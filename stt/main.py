"""Faster-Whisper STT HTTP service.

POST /stt   multipart: audio file → {"text": "...", "language": "en"}
GET  /health                      → {"status": "ok", "model": "..."}

The Whisper model (~77 MB for 'tiny') is downloaded from HuggingFace on first
start and persisted to the HF_HOME volume mount so restarts are instant.
"""

import logging
import os
import tempfile

from fastapi import FastAPI, File, UploadFile

logging.basicConfig(level=logging.INFO, format="%(levelname)s %(name)s %(message)s")
logger = logging.getLogger(__name__)

WHISPER_MODEL = os.getenv("WHISPER_MODEL", "tiny")
WHISPER_DEVICE = os.getenv("WHISPER_DEVICE", "cpu")
WHISPER_COMPUTE = os.getenv("WHISPER_COMPUTE", "int8")

logger.info("Loading Whisper model=%s device=%s compute=%s", WHISPER_MODEL, WHISPER_DEVICE, WHISPER_COMPUTE)
from faster_whisper import WhisperModel  # noqa: E402
_model = WhisperModel(WHISPER_MODEL, device=WHISPER_DEVICE, compute_type=WHISPER_COMPUTE)
logger.info("Whisper model ready")

app = FastAPI(title="STT Service")


@app.post("/stt")
async def transcribe(audio: UploadFile = File(...)):
    data = await audio.read()
    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as f:
        f.write(data)
        tmp = f.name
    try:
        segments, info = _model.transcribe(tmp, beam_size=1, language="en")
        text = " ".join(s.text.strip() for s in segments).strip()
        logger.info("transcribed len=%d lang=%s", len(text), info.language)
        return {"text": text, "language": info.language}
    finally:
        os.unlink(tmp)


@app.get("/health")
async def health():
    return {"status": "ok", "model": WHISPER_MODEL}
