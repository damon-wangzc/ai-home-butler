"""MCP (Model Context Protocol) client — SSE transport.

Discovers tools from external MCP servers and routes dispatch_tool() calls to them.

Config via environment variables:
  MCP_SERVERS  comma-separated SSE base-URLs
               e.g. http://fetch-mcp:8400,http://homeassistant.local:8123/mcp_server
  MCP_TOKENS   comma-separated Bearer tokens (same order as MCP_SERVERS).
               Leave a blank entry for servers that need no auth:
               e.g. ,myHAtoken   ← first server anonymous, second authenticated

At startup call connect_all() — it probes every server and builds an
OpenAI-schema list.  Per-call use call_mcp_tool() — it reconnects
briefly over SSE, calls the tool, and returns the result as a dict.

This module never touches MCP servers unless MCP_SERVERS is set.
"""

import logging
import os
from typing import Any

logger = logging.getLogger(__name__)

_SERVER_URLS: list[str] = [
    s.strip() for s in os.getenv("MCP_SERVERS", "").split(",") if s.strip()
]
_SERVER_TOKENS: list[str] = [
    t.strip() for t in os.getenv("MCP_TOKENS", "").split(",")
]

# tool_name → (base_url, bearer_token)
_tool_server: dict[str, tuple[str, str]] = {}
# OpenAI-format schemas discovered from MCP servers (populated by connect_all)
_discovered_schemas: list[dict] = []


def _sse_url(base: str) -> str:
    """Append /sse unless the URL already ends with it (e.g. HA's endpoint)."""
    base = base.rstrip("/")
    return base if base.endswith("/sse") else base + "/sse"


def _token_for(index: int) -> str:
    if index < len(_SERVER_TOKENS):
        return _SERVER_TOKENS[index]
    return ""


async def connect_all() -> list[dict]:
    """Connect to every configured MCP server and return OpenAI-format tool schemas.

    Called once at orchestrator startup inside the lifespan.
    Safe to call even when MCP_SERVERS is empty — returns [] immediately.
    """
    if not _SERVER_URLS:
        logger.info("MCP_SERVERS not configured — MCP integration disabled")
        return []

    try:
        from mcp import ClientSession
        from mcp.client.sse import sse_client
    except ImportError:
        logger.warning("'mcp' package not installed — MCP integration disabled")
        return []

    schemas: list[dict] = []
    for i, base_url in enumerate(_SERVER_URLS):
        token = _token_for(i)
        sse = _sse_url(base_url)
        headers: dict[str, str] = {"Authorization": f"Bearer {token}"} if token else {}
        try:
            async with sse_client(sse, headers=headers) as (read, write):
                async with ClientSession(read, write) as session:
                    await session.initialize()
                    tools_result = await session.list_tools()
                    for t in tools_result.tools:
                        _tool_server[t.name] = (base_url, token)
                        schemas.append({
                            "type": "function",
                            "function": {
                                "name": t.name,
                                "description": t.description or "",
                                "parameters": t.inputSchema,
                            },
                        })
                    logger.info(
                        "mcp_connected url=%s tools=%s",
                        base_url,
                        [t.name for t in tools_result.tools],
                    )
        except Exception as exc:
            # Non-fatal: log and continue — native tools still work
            logger.warning("mcp_connect_failed url=%s error=%s", base_url, exc)

    _discovered_schemas.extend(schemas)
    logger.info("mcp_tools_discovered total=%d", len(schemas))
    return schemas


async def call_mcp_tool(name: str, args: dict) -> Any:
    """Execute a tool on its registered MCP server.

    Reconnects per call (short-lived SSE session) — adds ~100-300 ms per
    call over LAN, acceptable for home butler latency.

    Raises ValueError if the tool name is not in the discovered registry.
    """
    entry = _tool_server.get(name)
    if entry is None:
        raise ValueError(
            f"Unknown MCP tool: {name!r}. "
            f"Discovered tools: {list(_tool_server)}"
        )
    base_url, token = entry

    from mcp import ClientSession
    from mcp.client.sse import sse_client

    sse = _sse_url(base_url)
    headers: dict[str, str] = {"Authorization": f"Bearer {token}"} if token else {}

    async with sse_client(sse, headers=headers) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()
            result = await session.call_tool(name, arguments=args)

    # Flatten MCP content list (TextContent, ImageContent, ...) into a single string
    texts = [c.text for c in result.content if hasattr(c, "text")]
    is_error = getattr(result, "isError", False)
    return {"result": "\n".join(texts), "is_error": is_error}


def is_mcp_tool(name: str) -> bool:
    """Return True if name was discovered from an MCP server."""
    return name in _tool_server
