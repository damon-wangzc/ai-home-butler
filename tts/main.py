"""Piper TTS HTTP service.

POST /tts   JSON {"text": "..."} → WAV bytes (audio/wav)
GET  /health                     → {"status": "ok", "voice": "..."}

The voice model (~67 MB) is downloaded from HuggingFace on first start and
persisted to VOICE_DIR (volume mount) so restarts are instant.
"""

import io
import logging
import os
import wave
from contextlib import asynccontextmanager

import httpx
import numpy as np
from fastapi import FastAPI
from fastapi.responses import Response
from pydantic import BaseModel

logging.basicConfig(level=logging.INFO, format="%(levelname)s %(name)s %(message)s")
logger = logging.getLogger(__name__)

VOICE_DIR = os.getenv("VOICE_DIR", "/data/tts-voices")
VOICE_NAME = os.getenv("VOICE_NAME", "en_US-lessac-medium")

# Download base URLs keyed by voice name.
# Files already present in VOICE_DIR are never re-downloaded.
_VOICE_BASE_URLS: dict[str, str] = {
    # English (US)
    "en_US-lessac-medium": (
        "https://huggingface.co/rhasspy/piper-voices/resolve/v1.0.0"
        "/en/en_US/lessac/medium"
    ),
    "en_US-amy-medium": (
        "https://huggingface.co/rhasspy/piper-voices/resolve/v1.0.0"
        "/en/en_US/amy/medium"
    ),
    # Swedish
    "sv_SE-nst-medium": (
        "https://huggingface.co/rhasspy/piper-voices/resolve/v1.0.0"
        "/sv/sv_SE/nst/medium"
    ),
    # Chinese (Simplified)
    "zh_CN-xiao_ya-medium": (
        "https://huggingface.co/rhasspy/piper-voices/resolve/v1.0.0"
        "/zh/zh_CN/xiao_ya/medium"
    ),
}

_voice = None


async def _ensure_voice(name: str) -> tuple[str, str]:
    """Download voice .onnx and .onnx.json if not already cached."""
    os.makedirs(VOICE_DIR, exist_ok=True)
    onnx_path = os.path.join(VOICE_DIR, f"{name}.onnx")
    json_path = os.path.join(VOICE_DIR, f"{name}.onnx.json")

    base_url = _VOICE_BASE_URLS.get(name)
    if not base_url:
        raise ValueError(f"Unknown voice '{name}'. Add it to _VOICE_BASE_URLS in tts/main.py")

    # verify=False: tolerate self-signed CA certs in corporate/WSL environments
    async with httpx.AsyncClient(timeout=120.0, follow_redirects=True, verify=False) as client:
        for url, path in [
            (f"{base_url}/{name}.onnx", onnx_path),
            (f"{base_url}/{name}.onnx.json", json_path),
        ]:
            if not os.path.exists(path):
                logger.info("Downloading %s ...", url)
                resp = await client.get(url)
                resp.raise_for_status()
                with open(path, "wb") as f:
                    f.write(resp.content)
                logger.info("  saved %s (%d bytes)", path, len(resp.content))

    return onnx_path, json_path


@asynccontextmanager
async def lifespan(app: FastAPI):
    global _voice
    onnx_path, json_path = await _ensure_voice(VOICE_NAME)
    from piper.voice import PiperVoice
    _voice = PiperVoice.load(onnx_path, config_path=json_path)
    logger.info("Piper voice loaded: %s", VOICE_NAME)
    yield


app = FastAPI(title="TTS Service", lifespan=lifespan)


class TTSRequest(BaseModel):
    text: str


@app.post("/tts")
async def synthesize(req: TTSRequest):
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)          # 16-bit PCM
        wf.setframerate(_voice.config.sample_rate)
        for chunk in _voice.synthesize(req.text):
            # audio_float_array is float32 in [-1, 1]; convert to int16 PCM bytes
            pcm = (chunk.audio_float_array * 32767).astype(np.int16).tobytes()
            wf.writeframes(pcm)
    logger.info("tts synthesized len=%d chars -> %d bytes", len(req.text), buf.tell())
    return Response(content=buf.getvalue(), media_type="audio/wav")


@app.get("/health")
async def health():
    return {"status": "ok", "voice": VOICE_NAME}
