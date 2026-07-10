import asyncio
import json
import logging
import os
from contextlib import asynccontextmanager, suppress

import aiomqtt
import aiosqlite
import httpx
from fastapi import FastAPI, HTTPException, WebSocket, WebSocketDisconnect
from pydantic import BaseModel

from adapter import build_messages, call_model, extract_response
from memory import get_memory_summary, store_turn
from tools import dispatch_tool, setup_db, register_mcp_tools, TOOL_SCHEMAS, DB_PATH
import mcp_client

STT_URL = os.getenv("STT_URL", "http://stt:8100")
TTS_URL = os.getenv("TTS_URL", "http://tts:8200")
MQTT_HOST = os.getenv("MQTT_HOST", "mosquitto")
MQTT_PORT = int(os.getenv("MQTT_PORT", "1883"))

logging.basicConfig(level=logging.INFO, format="%(levelname)s %(name)s %(message)s")
logger = logging.getLogger(__name__)

# Cost tracking rates (credits per 1M tokens; GPT-5 mini defaults)
_IN_RATE = float(os.getenv("IN_RATE_PER_1M", "25.0"))
_OUT_RATE = float(os.getenv("OUT_RATE_PER_1M", "200.0"))

# Active user — updated by MQTT butler/user/context messages (Phase 4)
_active_user_id: str = "default"


def _compute_cost(usage: dict) -> float:
    pt = usage.get("prompt_tokens", 0)
    ct = usage.get("completion_tokens", 0)
    return round((pt * _IN_RATE + ct * _OUT_RATE) / 1_000_000, 8)


def _merge_usage(a: dict, b: dict) -> dict:
    return {
        "prompt_tokens": a.get("prompt_tokens", 0) + b.get("prompt_tokens", 0),
        "completion_tokens": a.get("completion_tokens", 0) + b.get("completion_tokens", 0),
    }


# ── Phase 4: MQTT subscriber ──────────────────────────────────────────────────

async def _handle_mqtt(topic: str, payload: bytes) -> None:
    """Dispatch incoming MQTT messages."""
    global _active_user_id
    try:
        data = json.loads(payload.decode())
        if topic == "butler/user/context":
            # ESP32 orb publishes user_id; legacy devices may publish user
            user = (data.get("user_id") or data.get("user", "")).strip()
            if user:
                _active_user_id = user
                logger.info("mqtt_user_context user_id=%s source=%s",
                            user, data.get("source", "unknown"))
        elif topic == "butler/orb/touch":
            logger.info("orb_touch ts=%s active_user=%s",
                        data.get("ts", ""), _active_user_id)
    except Exception as exc:
        logger.warning("mqtt_handle_error topic=%s: %s", topic, exc)


async def _mqtt_listener() -> None:
    """Subscribe to butler/# and update orchestrator state. Reconnects on failure."""
    while True:
        try:
            async with aiomqtt.Client(MQTT_HOST, port=MQTT_PORT) as client:
                logger.info("MQTT connected host=%s port=%d", MQTT_HOST, MQTT_PORT)
                await client.subscribe("butler/#")
                async for msg in client.messages:
                    await _handle_mqtt(str(msg.topic), msg.payload)
        except aiomqtt.MqttError as exc:
            logger.warning("MQTT disconnected: %s — retry in 5s", exc)
            await asyncio.sleep(5)
        except asyncio.CancelledError:
            return
        except Exception as exc:
            logger.error("MQTT unexpected error: %s — retry in 10s", exc)
            await asyncio.sleep(10)


# ── Phase 5: Orb helpers ──────────────────────────────────────────────────────

async def _publish_orb_state(state: str, text: str = "", emotion: str = "") -> None:
    """Publish face/state update to butler/orb/state for the ESP32-S3 orb.

    Non-fatal — silently skipped if the MQTT broker is unreachable or the
    orb is not connected.
    """
    try:
        payload = json.dumps({
            "state": state,
            "text": text[:80] if text else "",
            "emotion": emotion,
        })
        async with aiomqtt.Client(MQTT_HOST, port=MQTT_PORT) as mc:
            await mc.publish("butler/orb/state", payload=payload.encode(), qos=0)
        logger.debug("orb_state state=%s", state)
    except Exception as exc:
        logger.debug("orb_state_publish_skip: %s", exc)


def _ensure_wav(audio_bytes: bytes,
                sample_rate: int = 16000,
                channels: int = 1,
                bit_depth: int = 16) -> bytes:
    """Wrap raw PCM bytes in a minimal WAV header if not already WAV.

    The ESP32-S3 orb streams raw 16-bit/16kHz/mono PCM; the STT service
    expects a WAV file. Pass-through if audio_bytes already starts with RIFF.
    """
    if len(audio_bytes) >= 4 and audio_bytes[:4] == b'RIFF':
        return audio_bytes
    import struct
    data_len = len(audio_bytes)
    byte_rate = sample_rate * channels * bit_depth // 8
    block_align = channels * bit_depth // 8
    header = struct.pack(
        '<4sI4s4sIHHIIHH4sI',
        b'RIFF', 36 + data_len, b'WAVE',
        b'fmt ', 16,
        1,            # PCM format
        channels,
        sample_rate,
        byte_rate,
        block_align,
        bit_depth,
        b'data', data_len,
    )
    return header + audio_bytes


# ── Phase 4: Reminder scheduler ───────────────────────────────────────────────

async def _reminder_scheduler() -> None:
    """Poll SQLite every 30 s for due reminders. Synthesizes TTS and saves WAV."""
    import datetime
    while True:
        try:
            await asyncio.sleep(30)
            now = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S")
            async with aiosqlite.connect(DB_PATH) as db:
                async with db.execute(
                    "SELECT id, user_id, message FROM reminders "
                    "WHERE due_iso <= ? AND done = 0",
                    (now,),
                ) as cur:
                    rows = await cur.fetchall()
                for (rid, uid, msg) in rows:
                    logger.info("reminder_due id=%d user_id=%s msg=%r", rid, uid, msg)
                    try:
                        async with httpx.AsyncClient(timeout=15.0) as hclient:
                            tts_resp = await hclient.post(
                                f"{TTS_URL}/tts", json={"text": msg}
                            )
                            if tts_resp.status_code == 200:
                                os.makedirs("/data/reminders", exist_ok=True)
                                wav_path = f"/data/reminders/{rid}.wav"
                                with open(wav_path, "wb") as f:
                                    f.write(tts_resp.content)
                                logger.info("reminder_saved path=%s", wav_path)
                                # Publish delivery notification so any MQTT listener can react
                                try:
                                    import json as _json
                                    async with aiomqtt.Client(MQTT_HOST, port=MQTT_PORT) as mc:
                                        await mc.publish(
                                            "butler/reminders/due",
                                            payload=_json.dumps({
                                                "id": rid,
                                                "user_id": uid,
                                                "message": msg,
                                                "wav": wav_path,
                                            }).encode(),
                                        )
                                    logger.info("reminder_mqtt_published id=%d", rid)
                                except Exception as mqtt_exc:
                                    logger.warning("reminder_mqtt_publish_error id=%d: %s", rid, mqtt_exc)
                    except Exception as exc:
                        logger.warning("reminder_tts_error id=%d: %s", rid, exc)
                    await db.execute("UPDATE reminders SET done = 1 WHERE id = ?", (rid,))
                await db.commit()
        except asyncio.CancelledError:
            return
        except Exception as exc:
            logger.error("scheduler_error: %s", exc)


@asynccontextmanager
async def lifespan(app: FastAPI):
    await setup_db()
    # Pre-warm ChromaDB embedding function at startup so the ONNX model is
    # downloaded/loaded before the first /ask request arrives.
    # The model is persisted to ./data/onnx-cache (via volume mount in compose.yml)
    # so subsequent container starts are instant.
    try:
        from chromadb.utils.embedding_functions import DefaultEmbeddingFunction
        # Download is synchronous — run in thread to avoid blocking the event loop
        await asyncio.to_thread(lambda: DefaultEmbeddingFunction()("warmup"))
        logger.info("ChromaDB embedding function warmed up")
    except Exception:
        logger.warning("ChromaDB embedding warmup failed — memory disabled", exc_info=True)

    # Start Phase 4 background tasks
    bg_tasks = [
        asyncio.create_task(_mqtt_listener(), name="mqtt_listener"),
        asyncio.create_task(_reminder_scheduler(), name="reminder_scheduler"),
    ]
    logger.info("Background tasks started: %s", [t.get_name() for t in bg_tasks])

    # MCP tool discovery — non-fatal if servers are unavailable
    mcp_schemas = await mcp_client.connect_all()
    if mcp_schemas:
        register_mcp_tools(mcp_schemas)
        logger.info("MCP tools available: %d", len(mcp_schemas))

    yield

    # Graceful shutdown
    for task in bg_tasks:
        task.cancel()
    await asyncio.gather(*bg_tasks, return_exceptions=True)
    logger.info("Background tasks stopped")


app = FastAPI(title="Butler Orchestrator", version="0.3.0", lifespan=lifespan)


class AskRequest(BaseModel):
    user_id: str
    text: str


async def _process_ask(user_id: str, text: str) -> dict:
    """Core ask logic — shared by /ask HTTP and /audio WebSocket.

    Returns the same dict as /ask.
    Raises ValueError on tool dispatch errors.
    """
    memory = await get_memory_summary(user_id, text)
    messages = build_messages(user_id, text, memory)
    raw = await call_model(messages, tools=TOOL_SCHEMAS)
    result = extract_response(raw)
    total_usage = raw.get("usage", {})

    if result["type"] == "call_function":
        fn_name = result["name"]
        fn_args = result.get("args", {})
        logger.info("function_call name=%s args=%s", fn_name, fn_args)
        tool_out = await dispatch_tool(fn_name, fn_args)  # raises ValueError on unknown tool

        raw_message = raw["choices"][0]["message"]
        if raw_message.get("tool_calls"):
            tc_id = raw_message["tool_calls"][0]["id"]
            messages.append(raw_message)
            messages.append({
                "role": "tool",
                "tool_call_id": tc_id,
                "content": json.dumps(tool_out),
            })
        else:
            messages.append({"role": "assistant", "content": json.dumps(result)})
            messages.append({
                "role": "user",
                "content": (
                    f"Tool result: {json.dumps(tool_out)}\n"
                    "Now give the user a concise answer."
                ),
            })

        raw2 = await call_model(messages)
        result = extract_response(raw2)
        total_usage = _merge_usage(total_usage, raw2.get("usage", {}))

    response_text = result.get("text", "")
    await store_turn(user_id, text, response_text)

    return {
        "user_id": user_id,
        "response": response_text,
        "cost_estimate": _compute_cost(total_usage),
        "usage": total_usage,
    }


@app.post("/ask")
async def ask(req: AskRequest):
    logger.info("ask user_id=%s text_len=%d", req.user_id, len(req.text))
    try:
        return await _process_ask(req.user_id, req.text)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc


@app.websocket("/audio")
async def audio(ws: WebSocket):
    """WebSocket audio endpoint — Phase 3/5.

    Client sends raw PCM or WAV bytes → STT → ask → TTS → server sends WAV bytes back.
    Server also sends JSON text frames to drive the ESP32-S3 orb face:
      {"type":"state","state":"thinking"}
      {"type":"state","state":"speaking","text":"..."}
      {"type":"end"}
    First message may optionally be a JSON text frame {"user_id": "...", "source": "..."}.
    Subsequent messages are binary audio frames (raw PCM 16kHz/16-bit/mono, or WAV).
    """
    await ws.accept()
    user_id = _active_user_id
    try:
        async with httpx.AsyncClient(timeout=30.0) as client:
            while True:
                data = await ws.receive()

                # Allow an initial JSON config frame
                if "text" in data:
                    try:
                        cfg = json.loads(data["text"])
                        user_id = cfg.get("user_id", user_id)
                    except json.JSONDecodeError:
                        pass
                    continue

                audio_bytes = data.get("bytes")
                if not audio_bytes:
                    continue

                # STT — wrap raw PCM in WAV header if needed (ESP32 orb sends raw PCM)
                stt_resp = await client.post(
                    f"{STT_URL}/stt",
                    files={"audio": ("audio.wav", _ensure_wav(audio_bytes), "audio/wav")},
                )
                stt_resp.raise_for_status()
                transcript = stt_resp.json().get("text", "")
                logger.info("stt user_id=%s transcript_len=%d", user_id, len(transcript))

                if not transcript.strip():
                    continue

                # Signal thinking state to orb face (MQTT + WebSocket JSON frame)
                await _publish_orb_state("thinking")
                with suppress(Exception):
                    await ws.send_text(json.dumps({"type": "state", "state": "thinking"}))

                # Ask orchestrator
                try:
                    result = await _process_ask(user_id, transcript)
                except ValueError as exc:
                    response_text = f"Sorry, tool error: {exc}"
                else:
                    response_text = result["response"]

                if not response_text.strip():
                    await _publish_orb_state("idle")
                    continue

                # Signal speaking state to orb face
                await _publish_orb_state("speaking", text=response_text)
                with suppress(Exception):
                    await ws.send_text(json.dumps({
                        "type": "state",
                        "state": "speaking",
                        "text": response_text[:80],
                    }))

                # TTS
                tts_resp = await client.post(
                    f"{TTS_URL}/tts",
                    json={"text": response_text},
                )
                tts_resp.raise_for_status()
                await ws.send_bytes(tts_resp.content)

                # Signal end of response
                with suppress(Exception):
                    await ws.send_text(json.dumps({"type": "end"}))
                await _publish_orb_state("idle")

    except WebSocketDisconnect:
        logger.info("audio WebSocket disconnected user_id=%s", user_id)


@app.get("/health")
async def health():
    return {"status": "ok"}
