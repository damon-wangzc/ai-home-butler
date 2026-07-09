"""Tool registry and dispatcher.

Phase 0: echo_tool (smoke-test the function-call loop).
Phase 1: query_portfolio, get_market_price (with TTL cache).
Phase 4: get_home_state, set_home_state, schedule_reminder.
Phase 6: detect_object (Frigate NVR), patrol_room (UGV via MQTT).

Security contract:
  - dispatch_tool() validates name against _REGISTRY whitelist before executing.
  - Tools NEVER receive raw user text; they receive structured args only.
  - Tools that touch external APIs read secrets from env, never from args.
"""

import logging
import os
import time
from typing import Any

import aiosqlite
import httpx

logger = logging.getLogger(__name__)

DB_PATH = os.getenv("DB_PATH", "/data/butler.db")
_ALPHA_VANTAGE_KEY = os.getenv("ALPHA_VANTAGE_KEY", "")
_PRICE_CACHE_TTL = float(os.getenv("PRICE_CACHE_TTL", "300"))  # seconds
_RAG_URL = os.getenv("RAG_URL", "http://rag:8300")
# Phase 4
_HA_URL = os.getenv("HA_URL", "")
_HA_TOKEN = os.getenv("HA_TOKEN", "")
# Phase 6
_FRIGATE_URL = os.getenv("FRIGATE_URL", "http://frigate:5000")
_MQTT_HOST = os.getenv("MQTT_HOST", "mosquitto")
_MQTT_PORT = int(os.getenv("MQTT_PORT", "1883"))

_REGISTRY: dict[str, Any] = {}
_price_cache: dict[str, tuple[float, float]] = {}  # symbol → (price, monotonic_ts)


def tool(fn: Any) -> Any:
    """Decorator: register an async function as a dispatchable tool."""
    _REGISTRY[fn.__name__] = fn
    return fn


# ── Phase 1: Database setup ────────────────────────────────────────────────────

async def setup_db() -> None:
    """Create tables and seed demo portfolio data on first start."""
    async with aiosqlite.connect(DB_PATH) as db:
        await db.execute("PRAGMA journal_mode=WAL")
        await db.execute("""
            CREATE TABLE IF NOT EXISTS portfolio (
                user_id   TEXT NOT NULL,
                symbol    TEXT NOT NULL,
                qty       REAL NOT NULL,
                avg_price REAL NOT NULL,
                PRIMARY KEY (user_id, symbol)
            )
        """)
        await db.execute("""
            CREATE TABLE IF NOT EXISTS reminders (
                id       INTEGER PRIMARY KEY AUTOINCREMENT,
                user_id  TEXT    NOT NULL,
                message  TEXT    NOT NULL,
                due_iso  TEXT    NOT NULL,
                done     INTEGER NOT NULL DEFAULT 0,
                created  TEXT    NOT NULL DEFAULT (datetime('now'))
            )
        """)
        await db.executemany(
            "INSERT OR IGNORE INTO portfolio (user_id, symbol, qty, avg_price) "
            "VALUES (?, ?, ?, ?)",
            [
                ("default", "AAPL",  10.0,  150.00),
                ("default", "MSFT",   5.0,  300.00),
                ("default", "GOOGL",  2.0, 2500.00),
            ],
        )
        await db.commit()
    logger.info("setup_db complete db_path=%s", DB_PATH)


# ── Tools ──────────────────────────────────────────────────────────────────────

@tool
async def echo_tool(message: str = "", **_: Any) -> dict:
    """Phase 0 smoke-test — echo the message back."""
    logger.info("echo_tool message=%r", message)
    return {"echo": message}


@tool
async def query_portfolio(user_id: str = "default", **_: Any) -> dict:
    """Return the user's investment holdings from the local SQLite database."""
    async with aiosqlite.connect(DB_PATH) as db:
        async with db.execute(
            "SELECT symbol, qty, avg_price FROM portfolio "
            "WHERE user_id = ? ORDER BY symbol",
            (user_id,),
        ) as cursor:
            rows = await cursor.fetchall()
    holdings = [{"symbol": r[0], "qty": r[1], "avg_price": r[2]} for r in rows]
    logger.info("query_portfolio user_id=%s holdings=%d", user_id, len(holdings))
    return {"user_id": user_id, "holdings": holdings}


@tool
async def get_market_price(symbol: str = "", **_: Any) -> dict:
    """Fetch the current market price for a stock symbol via Alpha Vantage.

    Results are cached in-process for PRICE_CACHE_TTL seconds (default 300 s).
    Requires ALPHA_VANTAGE_KEY env var (free tier: 25 req/day at alphavantage.co).
    """
    symbol = symbol.upper().strip()
    if not symbol:
        raise ValueError("symbol is required")

    now = time.monotonic()
    cached = _price_cache.get(symbol)
    if cached and (now - cached[1]) < _PRICE_CACHE_TTL:
        logger.info("get_market_price cache_hit symbol=%s", symbol)
        return {"symbol": symbol, "price": cached[0], "cached": True}

    if not _ALPHA_VANTAGE_KEY:
        raise ValueError(
            "ALPHA_VANTAGE_KEY env var not set — "
            "get a free key at https://www.alphavantage.co/support/#api-key"
        )

    async with httpx.AsyncClient(timeout=10.0) as client:
        resp = await client.get(
            "https://www.alphavantage.co/query",
            params={
                "function": "GLOBAL_QUOTE",
                "symbol": symbol,
                "apikey": _ALPHA_VANTAGE_KEY,
            },
        )
        resp.raise_for_status()
        data = resp.json()

    price_str = data.get("Global Quote", {}).get("05. price", "")
    if not price_str:
        raise ValueError(
            f"No price data returned for '{symbol}' — check symbol or API limit"
        )

    price = float(price_str)
    _price_cache[symbol] = (price, now)
    logger.info("get_market_price symbol=%s price=%s", symbol, price)
    return {"symbol": symbol, "price": price, "cached": False}


@tool
async def query_home_knowledge(query: str = "", **_: Any) -> dict:
    """Search the home knowledge base for relevant information.

    Use this tool to answer questions about the home, appliances, schedules,
    or anything the user has stored in the knowledge base.
    """
    if not query:
        raise ValueError("query is required")
    async with httpx.AsyncClient(timeout=10.0) as client:
        resp = await client.post(
            f"{_RAG_URL}/v1/query",
            json={"query": query, "top_k": 3},
        )
        resp.raise_for_status()
    data = resp.json()
    results = data.get("results", [])
    logger.info("query_home_knowledge query=%r hits=%d", query, len(results))
    return {"query": query, "results": results}


# ── Phase 4: Home Assistant ───────────────────────────────────────────────────

@tool
async def get_home_state(entity_ids: list = None, **_: Any) -> dict:
    """Get current state of one or more Home Assistant entities (lights, sensors,
    locks, thermostats, etc.).

    Returns state and attributes for each entity.
    Requires HA_URL and HA_TOKEN env vars.
    """
    entity_ids = entity_ids or []
    if not _HA_URL or not _HA_TOKEN:
        return {
            "error": "HA_URL and HA_TOKEN not configured — set them in .env",
            "entities": [],
        }
    headers = {
        "Authorization": f"Bearer {_HA_TOKEN}",
        "Content-Type": "application/json",
    }
    states = []
    async with httpx.AsyncClient(timeout=10.0) as client:
        for eid in entity_ids:
            resp = await client.get(
                f"{_HA_URL}/api/states/{eid}",
                headers=headers,
            )
            if resp.status_code == 200:
                data = resp.json()
                states.append({
                    "entity_id": eid,
                    "state": data.get("state"),
                    "attributes": data.get("attributes", {}),
                })
            else:
                states.append({"entity_id": eid, "error": f"HTTP {resp.status_code}"})
    logger.info("get_home_state entity_ids=%s", entity_ids)
    return {"entities": states}


@tool
async def set_home_state(entity_id: str = "", state: str = "", **_: Any) -> dict:
    """Control a Home Assistant entity — turn lights on/off, set thermostat, lock/unlock, etc.

    Examples:
      entity_id="light.living_room" state="on"
      entity_id="switch.garden"     state="off"
      entity_id="climate.bedroom"   state="heat"

    Requires HA_URL and HA_TOKEN env vars.
    """
    if not entity_id:
        raise ValueError("entity_id is required")
    if not state:
        raise ValueError("state is required")
    if not _HA_URL or not _HA_TOKEN:
        return {"error": "HA_URL and HA_TOKEN not configured — set them in .env"}

    domain = entity_id.split(".")[0] if "." in entity_id else "homeassistant"
    # Map state string to HA service name
    _SERVICE_MAP = {
        "on": "turn_on", "off": "turn_off", "true": "turn_on", "false": "turn_off",
        "lock": "lock", "unlock": "unlock", "open": "open_cover", "close": "close_cover",
    }
    service = _SERVICE_MAP.get(state.lower(), state.lower())

    headers = {
        "Authorization": f"Bearer {_HA_TOKEN}",
        "Content-Type": "application/json",
    }
    async with httpx.AsyncClient(timeout=10.0) as client:
        resp = await client.post(
            f"{_HA_URL}/api/services/{domain}/{service}",
            headers=headers,
            json={"entity_id": entity_id},
        )
        resp.raise_for_status()

    logger.info("set_home_state entity_id=%s state=%s", entity_id, state)
    return {"entity_id": entity_id, "new_state": state, "ok": True}


@tool
async def schedule_reminder(
    message: str = "", when_iso: str = "", user_id: str = "default", **_: Any
) -> dict:
    """Schedule a spoken reminder to be delivered at a specific date and time.

    Args:
        message:  What to say when the reminder fires.
        when_iso: ISO 8601 datetime in UTC, e.g. '2026-07-08T18:00:00'.
        user_id:  Who to remind (default: current user).
    """
    if not message:
        raise ValueError("message is required")
    if not when_iso:
        raise ValueError("when_iso is required (ISO 8601 datetime)")

    async with aiosqlite.connect(DB_PATH) as db:
        cur = await db.execute(
            "INSERT INTO reminders (user_id, message, due_iso) VALUES (?, ?, ?)",
            (user_id, message, when_iso),
        )
        rid = cur.lastrowid
        await db.commit()

    logger.info("schedule_reminder id=%d user_id=%s due=%s", rid, user_id, when_iso)
    return {"id": rid, "message": message, "due_iso": when_iso, "user_id": user_id}


# ── Phase 6: Robotics / Vision ────────────────────────────────────────────────

@tool
async def detect_object(camera: str = "all", **_: Any) -> dict:
    """Query Frigate NVR for recently detected objects across one or all cameras.

    Returns a list of recent detections: label, camera, confidence score.
    Requires FRIGATE_URL env var pointing to the Frigate instance.
    """
    try:
        params: dict[str, Any] = {"limit": 10}
        if camera != "all":
            params["camera"] = camera
        async with httpx.AsyncClient(timeout=5.0) as client:
            resp = await client.get(f"{_FRIGATE_URL}/api/events", params=params)
            resp.raise_for_status()
            events = resp.json()
        objects = [
            {
                "label": e.get("label"),
                "camera": e.get("camera"),
                "score": round(e.get("top_score", 0), 3),
            }
            for e in (events if isinstance(events, list) else [])
        ]
        logger.info("detect_object camera=%s objects=%d", camera, len(objects))
        return {"camera": camera, "objects": objects}
    except httpx.ConnectError:
        return {"camera": camera, "objects": [], "error": "Frigate not reachable — is it running?"}
    except Exception as exc:
        logger.warning("detect_object error: %s", exc)
        return {"camera": camera, "objects": [], "error": str(exc)}


@tool
async def patrol_room(room: str = "", **_: Any) -> dict:
    """Send the UGV rover to patrol or check a specific room.

    Publishes a navigation command to the MQTT topic butler/ugv/navigate.
    The UGV firmware subscribes to this topic and drives to the specified room.

    Requires the mosquitto MQTT broker to be running.
    """
    import json as _json
    if not room:
        raise ValueError("room is required")

    payload = _json.dumps({"room": room, "command": "patrol"}).encode()
    try:
        import aiomqtt
        async with aiomqtt.Client(_MQTT_HOST, port=_MQTT_PORT) as client:
            await client.publish("butler/ugv/navigate", payload=payload)
        logger.info("patrol_room room=%s queued=True", room)
        return {"room": room, "command": "patrol", "queued": True}
    except Exception as exc:
        logger.warning("patrol_room mqtt_error: %s", exc)
        return {"room": room, "command": "patrol", "queued": False, "error": str(exc)}


# ── MCP tool support ──────────────────────────────────────────────────────────

# Names of tools discovered from external MCP servers (populated at startup)
_MCP_TOOL_NAMES: set[str] = set()


def register_mcp_tools(schemas: list[dict]) -> None:
    """Extend TOOL_SCHEMAS in-place with tools discovered from MCP servers.

    Called once during lifespan startup after mcp_client.connect_all().
    """
    for schema in schemas:
        name = schema.get("function", {}).get("name", "")
        if name:
            _MCP_TOOL_NAMES.add(name)
            TOOL_SCHEMAS.append(schema)
    logger.info("mcp_tools_registered names=%s", sorted(_MCP_TOOL_NAMES))


async def dispatch_tool(name: str, args: dict) -> Any:
    """Execute a registered tool by name.

    Checks native _REGISTRY first, then MCP tools.
    Raises ValueError for any name not in either whitelist.
    """
    if name in _REGISTRY:
        logger.info("dispatch_tool native name=%s args=%s", name, args)
        return await _REGISTRY[name](**args)
    if name in _MCP_TOOL_NAMES:
        import mcp_client
        logger.info("dispatch_tool mcp name=%s args=%s", name, args)
        return await mcp_client.call_mcp_tool(name, args)
    raise ValueError(f"Unknown tool: {name!r}. Native: {list(_REGISTRY)} MCP: {sorted(_MCP_TOOL_NAMES)}")


# ── OpenAI-style tool schemas (passed to model for native function calling) ────

TOOL_SCHEMAS: list[dict] = [
    {
        "type": "function",
        "function": {
            "name": "echo_tool",
            "description": "Echo the message back. Use for smoke-testing the function-call loop.",
            "parameters": {
                "type": "object",
                "properties": {
                    "message": {
                        "type": "string",
                        "description": "Message to echo.",
                    }
                },
                "required": ["message"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "query_portfolio",
            "description": "Get the user's investment portfolio holdings.",
            "parameters": {
                "type": "object",
                "properties": {
                    "user_id": {
                        "type": "string",
                        "description": "User identifier. Use 'default' if unknown.",
                    }
                },
                "required": [],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "get_market_price",
            "description": "Fetch the current market price for a stock ticker symbol.",
            "parameters": {
                "type": "object",
                "properties": {
                    "symbol": {
                        "type": "string",
                        "description": "Stock ticker symbol, e.g. 'AAPL'.",
                    }
                },
                "required": ["symbol"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "query_home_knowledge",
            "description": "Search the home knowledge base for information about the home, appliances, schedules, or any documents the user has stored.",
            "parameters": {
                "type": "object",
                "properties": {
                    "query": {
                        "type": "string",
                        "description": "Natural language search query.",
                    }
                },
                "required": ["query"],
            },
        },
    },
    # Phase 4: Smart Home
    {
        "type": "function",
        "function": {
            "name": "get_home_state",
            "description": "Get the current state of one or more Home Assistant entities such as lights, sensors, locks, thermostats, or switches.",
            "parameters": {
                "type": "object",
                "properties": {
                    "entity_ids": {
                        "type": "array",
                        "items": {"type": "string"},
                        "description": "List of HA entity IDs, e.g. ['light.living_room', 'sensor.temperature']",
                    }
                },
                "required": ["entity_ids"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "set_home_state",
            "description": "Control a Home Assistant entity: turn lights on/off, set switches, lock/unlock doors, open/close covers.",
            "parameters": {
                "type": "object",
                "properties": {
                    "entity_id": {
                        "type": "string",
                        "description": "HA entity ID to control, e.g. 'light.living_room'",
                    },
                    "state": {
                        "type": "string",
                        "description": "Desired state: 'on', 'off', 'lock', 'unlock', 'open', 'close'",
                    },
                },
                "required": ["entity_id", "state"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "schedule_reminder",
            "description": "Schedule a spoken reminder to be delivered at a specific date and time.",
            "parameters": {
                "type": "object",
                "properties": {
                    "message": {
                        "type": "string",
                        "description": "Message to speak when the reminder fires.",
                    },
                    "when_iso": {
                        "type": "string",
                        "description": "Delivery time as ISO 8601 UTC datetime, e.g. '2026-07-08T18:00:00'",
                    },
                    "user_id": {
                        "type": "string",
                        "description": "User to remind. Defaults to current user.",
                    },
                },
                "required": ["message", "when_iso"],
            },
        },
    },
    # Phase 6: Robotics & Vision
    {
        "type": "function",
        "function": {
            "name": "detect_object",
            "description": "Query Frigate NVR for recently detected objects (people, animals, packages) across cameras.",
            "parameters": {
                "type": "object",
                "properties": {
                    "camera": {
                        "type": "string",
                        "description": "Camera name to query, or 'all' for all cameras.",
                    }
                },
                "required": [],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "patrol_room",
            "description": "Send the UGV rover to patrol or check a specific room.",
            "parameters": {
                "type": "object",
                "properties": {
                    "room": {
                        "type": "string",
                        "description": "Room to patrol, e.g. 'living_room', 'front_door', 'kitchen'",
                    }
                },
                "required": ["room"],
            },
        },
    },
]
