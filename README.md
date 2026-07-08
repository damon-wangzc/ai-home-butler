# AI Home Butler

A local AI assistant stack running entirely on your own hardware.
Text in → model reasons → tools execute → answer out. No cloud required.

**Current phase**: Phase 4 complete (RAG home knowledge base).

---

## Services

| Service | Image | Port | Purpose |
|---------|-------|------|---------|
| `model-server` | `ghcr.io/ggml-org/llama.cpp:server-cuda` | 8080 | OpenAI-compat LLM inference |
| `orchestrator` | `localhost/ai-home-butler_orchestrator` | 8000 | FastAPI — `/ask` HTTP, `/audio` WebSocket |
| `chromadb` | `chromadb/chroma:latest` | 8001 | Vector memory store (top-k=3 retrieval) |
| `stt` | `localhost/ai-home-butler_stt` | 8100 | Faster-Whisper speech-to-text |
| `tts` | `localhost/ai-home-butler_tts` | 8200 | Piper text-to-speech (WAV out) |
| `rag` | `localhost/ai-home-butler_rag` | 8300 | Home knowledge RAG (ChromaDB + ONNX) |

All persistent data lives in `./data/` and survives container restarts.

---

## Quick Start

```bash
# 1. Copy and fill in your secrets
cp .env.example .env
# edit .env — set ALPHA_VANTAGE_KEY (free key: https://www.alphavantage.co/support/#api-key)

# 2. Boot the stack (first run builds the orchestrator image)
podman-compose up --build -d

# 3. Wait for startup (ChromaDB ONNX model loads on first boot, ~30 s)
podman logs -f ai-home-butler_orchestrator_1
# Ready when: "Application startup complete."

# 4. Smoke test
curl -s http://127.0.0.1:8000/health
curl -s -X POST http://127.0.0.1:8000/ask \
  -H 'Content-Type: application/json' \
  -d '{"user_id":"default","text":"How is our AAPL position?"}' \
  | python3 -m json.tool
```

Expected response:
```json
{
    "user_id": "default",
    "response": "You hold 10 shares of AAPL ...",
    "cost_estimate": 0.0000xx,
    "usage": { "prompt_tokens": ..., "completion_tokens": ... }
}
```

---

## Environment Variables

Copy `.env.example` to `.env` and adjust:

| Variable | Default | Description |
|----------|---------|-------------|
| `MODEL_FILE` | `Qwen3.5-2B-Q4_K_M.gguf` | GGUF model filename (in `../models/`) |
| `N_GPU_LAYERS` | `99` | GPU layers (`0` = CPU-only, `99` = all layers) |
| `MAX_TOKENS` | `1024` | Max completion tokens (must be ≥ 1024 for Qwen3) |
| `ALPHA_VANTAGE_KEY` | *(empty)* | Free market price API key — required for `get_market_price` |
| `PRICE_CACHE_TTL` | `300` | Market price cache TTL in seconds |
| `IN_RATE_PER_1M` | `25.0` | Cost tracking: input token rate (credits/1M) |
| `OUT_RATE_PER_1M` | `200.0` | Cost tracking: output token rate (credits/1M) |
| `WHISPER_MODEL` | `tiny` | Faster-Whisper model size (`tiny`, `base`, `small`, `medium`) |
| `WHISPER_DEVICE` | `cpu` | STT inference device (`cpu` or `cuda`) |
| `WHISPER_COMPUTE` | `int8` | STT compute type (`int8`, `float16`) |
| `VOICE_NAME` | `en_US-lessac-medium` | Piper voice; must be in `_VOICE_BASE_URLS` in `tts/main.py` |

---

## Available Tools

The orchestrator exposes two real tools and one smoke-test stub:

| Tool | Source | Notes |
|------|--------|-------|
| `query_portfolio(user_id)` | `./data/butler.db` SQLite | Returns holdings; seeded with AAPL/MSFT/GOOGL demo data |
| `get_market_price(symbol)` | Alpha Vantage API | Cached for `PRICE_CACHE_TTL` seconds; requires API key |
| `echo_tool(message)` | in-process | Phase 0 smoke-test stub |

Tool schemas are passed to the model via the OpenAI `tools` parameter. The model picks the right tool; the orchestrator validates the name against a whitelist before executing.

---

## Data Layout

```
data/
├── butler.db          SQLite — portfolio table (WAL mode)
│                      Seeded: AAPL×10 @ $150, MSFT×5 @ $300, GOOGL×2 @ $2500
├── chroma/            ChromaDB persistent storage (per-user conversation memory)
├── onnx-cache/        ChromaDB ONNX embedding model — downloaded once, persisted here
├── whisper-cache/     Faster-Whisper model cache (HF_HOME mount)
└── tts-voices/        Piper ONNX voice model — downloaded on first TTS service start
```

---

## Useful Commands

```bash
# Rebuild only the orchestrator (model-server stays running)
podman-compose up --build -d orchestrator

# Follow live logs
podman logs -f ai-home-butler_orchestrator_1

# Tear down (data/volumes preserved)
podman-compose down

# Tear down AND wipe all data
podman-compose down && rm -rf data/butler.db data/chroma data/onnx-cache

# Test market price tool directly
curl -s -X POST http://127.0.0.1:8000/ask \
  -H 'Content-Type: application/json' \
  -d '{"user_id":"default","text":"What is the current price of MSFT?"}'

# Inspect the SQLite portfolio
sqlite3 data/butler.db "SELECT * FROM portfolio;"

# Run all smoke tests (Phases 0, 1, 3)
bash smoke_test.sh
```

---

## Audio (Phase 3)

### Service health checks
```bash
curl http://127.0.0.1:8100/health   # STT — {"status":"ok","model":"tiny"}
curl http://127.0.0.1:8200/health   # TTS — {"status":"ok","voice":"en_US-lessac-medium"}
```

### Test TTS directly (text → WAV file)
```bash
curl -s -X POST http://127.0.0.1:8200/tts \
  -H 'Content-Type: application/json' \
  -d '{"text": "Hello, I am your home butler."}' \
  -o /tmp/butler.wav
aplay /tmp/butler.wav   # or: ffplay -nodisp -autoexit /tmp/butler.wav
```

### Test STT directly (WAV file → transcript)
```bash
# Record a short clip first (requires arecord)
arecord -d 3 -f cd /tmp/test.wav
curl -s -X POST http://127.0.0.1:8100/stt \
  -F 'file=@/tmp/test.wav' | python3 -m json.tool
```

### WebSocket audio round-trip
```bash
# Requires: pip install websockets
python3 - <<'EOF'
import asyncio, websockets, pathlib

async def test():
    uri = "ws://127.0.0.1:8000/audio"
    wav = pathlib.Path("/tmp/test.wav").read_bytes()  # your WAV file
    async with websockets.connect(uri) as ws:
        import json
        await ws.send(json.dumps({"user_id": "default"}))
        await ws.send(wav)
        response_wav = await ws.recv()
        pathlib.Path("/tmp/response.wav").write_bytes(response_wav)
        print(f"Received {len(response_wav)} bytes of TTS audio")

asyncio.run(test())
EOF
aplay /tmp/response.wav
```

### Model size guide

| `WHISPER_MODEL` | Size | WSL speed | Notes |
|-----------------|------|-----------|-------|
| `tiny` | 77 MB | ~5–10× real-time | Default — fast, good enough for home use |
| `base` | 145 MB | ~3–5× | Better accuracy |
| `small` | 466 MB | ~1–2× | Slow on CPU; use on GPU |
| `medium` | 1.5 GB | <1× | GPU only |

Set in `.env`: `WHISPER_MODEL=base`  (restart stt container to take effect)

| Phase | Status | Goal |
|-------|--------|------|
| 0 — Foundation | ✓ Done | `/ask` endpoint, one tool, smoke test |
| 1 — Memory & Tools | ✓ Done | ChromaDB memory, portfolio + market price tools, cost tracking |
| 3 — Audio | ✅ Done | Whisper STT + Piper TTS over WebSocket |
| 4 — RAG | ⚡ In progress | Home knowledge base, auto-indexes `data/home-docs/` |
| 4 — Smart Home | Planned | Home Assistant tools via MQTT (develop on WSL) |
| 5 — Edge Devices | Planned | ESP32 voice orb + P4 dashboard (point at WSL IP during dev) |
| 6 — Robotics | Planned | UGV rover + Frigate NVR (develop on WSL) |
| 2 — DELL Migration | Deferred (after Phase 6) | Deploy complete tested stack to DELL — `.env` change only |
| 7 — Mac Mini | Planned | Final migration to Docker + M4 Metal |

See [`.github/instructions/roadmap.md`](../.github/instructions/roadmap.md) for full task lists.

---

## Knowledge Base (Phase 4)

Butler indexes `.txt` and `.md` files from `data/home-docs/` on startup and
makes them searchable via the `query_home_knowledge` tool.

### Add documents
```bash
# Drop any text/markdown file — it's indexed automatically on next restart
cp ~/my-appliance-manual.txt data/home-docs/
podman-compose restart rag
```

### Query the knowledge base directly
```bash
curl -s -X POST http://127.0.0.1:8300/v1/query \
  -H 'Content-Type: application/json' \
  -d '{"query": "how do I reset the dishwasher?", "top_k": 3}' \
  | python3 -m json.tool
```

### Index a document without restarting
```bash
curl -s -X POST http://127.0.0.1:8300/v1/index \
  -H 'Content-Type: application/json' \
  -d '{"source": "note.txt", "content": "The WiFi password is posted on the fridge."}' \
  | python3 -m json.tool
```

### Check chunk count
```bash
curl -s http://127.0.0.1:8300/health
# {"status": "ok", "chunks": 12}
```

Butler automatically calls `query_home_knowledge` when you ask about home-related topics.

