# AI Home Butler

A local AI assistant stack running entirely on your own hardware.
Text in → model reasons → tools execute → answer out. No cloud required.

**Current phase**: Phase 4 complete (RAG home knowledge base).

---

## Architecture

```mermaid
graph TD
    subgraph Edge["Edge Devices (Future)"]
        ESP32_ORB["ESP32-S3 Voice Orb\n(Wake word + mic)"]
        ESP32_P4["ESP32-P4 Command Panel\n(7\" Touch + Camera)"]
    end

    subgraph Core["AI Core — Podman Stack (WSL / DELL / Mac)"]
        ORCH["Orchestrator :8000\nFastAPI /ask · /audio WS"]
        MODEL["model-server :8080\nllama.cpp (GGUF)"]
        CHROMA["chromadb :8001\nVector Memory (ChromaDB)"]
        STT["stt :8100\nFaster-Whisper"]
        TTS["tts :8200\nPiper TTS"]
        RAG["rag :8300\nHome Knowledge RAG"]
        MQTT["mosquitto :1883\nMQTT Broker"]
        FETCH_MCP["fetch-mcp :8400\nMCP fetch_url tool"]
    end

    subgraph Data["Persistent Data  ./data/"]
        DB[("butler.db\nSQLite portfolio")]
        CHROMADIR[("chroma/\nVector store")]
        DOCS[("home-docs/\nMarkdown / PDF")]
        VOICES[("tts-voices/\nPiper ONNX")]
        WHISPER[("whisper-cache/\nHF model cache")]
    end

    subgraph External["External (optional)"]
        AV["Alpha Vantage API\n(market prices)"]
        HA["Home Assistant\n(smart home hub)"]
        HA_MCP["Home Assistant MCP\n(:8123/api/mcp_server/sse)"]
        WEB["Public Web\n(weather, APIs)"]
    end

    %% Edge → Core
    ESP32_ORB -- "WebSocket audio" --> ORCH
    ESP32_P4 -- "WebSocket audio\nMQTT user context" --> ORCH
    ESP32_P4 -- "MQTT face detect" --> MQTT

    %% Orchestrator internal calls
    ORCH -- "POST /v1/chat/completions" --> MODEL
    ORCH -- "top-k memory" --> CHROMA
    ORCH -- "POST /query" --> RAG
    ORCH -- "POST /stt" --> STT
    ORCH -- "POST /tts" --> TTS
    ORCH -- "PUBLISH butler/cmd" --> MQTT

    %% MCP tool dispatch
    ORCH -- "MCP SSE discovery + calls" --> FETCH_MCP
    ORCH -- "MCP SSE (optional)" --> HA_MCP
    FETCH_MCP -- "HTTP GET" --> WEB

    %% RAG → data
    RAG --> CHROMADIR
    RAG --> DOCS

    %% Data mounts
    ORCH --> DB
    CHROMA --> CHROMADIR
    TTS --> VOICES
    STT --> WHISPER

    %% External
    ORCH -- "tool: get_market_price" --> AV
    MQTT -- "REST / MQTT" --> HA
```

---

## Request Flows

### HTTP `/ask` — Text request

```mermaid
sequenceDiagram
    participant C as Client
    participant O as Orchestrator
    participant Ch as ChromaDB
    participant R as RAG
    participant M as model-server
    participant DB as SQLite
    participant AV as Alpha Vantage

    C->>O: POST /ask {user_id, text}
    O->>Ch: query top-3 memories (user_id)
    Ch-->>O: memory snippets
    O->>R: POST /query {text}
    R-->>O: home-doc context (top-k chunks)
    O->>M: /v1/chat/completions (system+memory+rag+user, tools=[])
    M-->>O: tool_call: query_portfolio | get_market_price | answer
    alt tool_call: query_portfolio
        O->>DB: SELECT * FROM portfolio WHERE user_id=?
        DB-->>O: holdings
        O->>M: follow-up with tool result
        M-->>O: final answer text
    else tool_call: get_market_price
        O->>AV: GET /query?symbol=…
        AV-->>O: price (cached TTL=300s)
        O->>M: follow-up with tool result
        M-->>O: final answer text
    end
    O->>Ch: upsert turn to memory
    O-->>C: {response, cost_estimate, usage}
```

### WebSocket `/audio` — Voice round-trip

```mermaid
sequenceDiagram
    participant D as Device (ESP32 / client)
    participant O as Orchestrator
    participant S as STT (Whisper)
    participant T as TTS (Piper)

    D->>O: WS connect → send {user_id}
    D->>O: send WAV bytes
    O->>S: POST /stt (WAV)
    S-->>O: {transcript}
    Note over O: runs same /ask logic internally
    O->>T: POST /tts {text: response}
    T-->>O: WAV bytes
    O-->>D: send WAV bytes
```

---

## Services

| Service | Image | Port | Purpose |
|---------|-------|------|---------|
| `model-server` | `ghcr.io/ggml-org/llama.cpp:server-cuda` | 8080 | OpenAI-compat LLM inference |
| `orchestrator` | `localhost/ai-home-butler_orchestrator` | 8000 | FastAPI — `/ask` HTTP, `/audio` WebSocket |
| `chromadb` | `chromadb/chroma:latest` | 8001 | Vector memory store (top-k=3 retrieval) |
| `stt` | `localhost/ai-home-butler_stt`<br>*(base: `nvidia/cuda:13.3.0-runtime-ubuntu26.04`)* | 8100 | Faster-Whisper speech-to-text (GPU via `WHISPER_DEVICE=cuda`) |
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
| `KV_CACHE_TYPE` | `f16` | llama.cpp KV cache precision (`f16`, `q8_0`, `q4_0`); use `q4_0` on P1000 for ctx ≥ 16K |
| `WHISPER_DEVICE` | `cpu` | STT inference device (`cpu` or `cuda`); set `cuda` on DELL P1000 |
| `WHISPER_COMPUTE` | `int8` | STT compute type (`int8` for CPU, `float16` for CUDA) |
| `VOICE_NAME` | `en_US-lessac-medium` | Piper voice; must be in `_VOICE_BASE_URLS` in `tts/main.py` |

---

## Available Tools

The orchestrator exposes native tools (registered in `tools.py`) plus any tools discovered from MCP servers at startup:

| Tool | Type | Source | Notes |
|------|------|--------|-------|
| `query_portfolio(user_id)` | native | `./data/butler.db` SQLite | Holdings; seeded with AAPL/MSFT/GOOGL |
| `get_market_price(symbol)` | native | Alpha Vantage API | Cached `PRICE_CACHE_TTL` s; needs API key |
| `get_home_state(entity_ids)` | native | Home Assistant REST | Requires `HA_URL` + `HA_TOKEN` |
| `set_home_state(entity_id, state)` | native | Home Assistant REST | Controls lights, locks, switches |
| `schedule_reminder(message, when_iso)` | native | SQLite + TTS | Fires spoken reminder at due time |
| `query_home_knowledge(query)` | native | RAG service `:8300` | Searches `data/home-docs/` |
| `detect_object(camera)` | native | Frigate NVR | Phase 6 — vision |
| `patrol_room(room)` | native | MQTT `butler/ugv/navigate` | Phase 7 — robotics |
| `fetch_url(url)` | **MCP** | `fetch-mcp :8400` | Real-time web content |
| *(any HA MCP tool)* | **MCP** | HA built-in MCP server | See MCP section below |

Tool schemas are passed to the model via the OpenAI `tools` parameter. The model picks the right tool; the orchestrator validates the name before executing.

---

## MCP Integration

The stack uses [Model Context Protocol (MCP)](https://modelcontextprotocol.io) to extend the butler's tool set without modifying `tools.py`. Any MCP server that speaks the SSE transport can be added.

### How it works

```mermaid
sequenceDiagram
    participant O as Orchestrator
    participant M as MCP Server (SSE)

    Note over O: startup — lifespan()
    O->>M: GET /sse (initialize)
    M-->>O: list_tools response
    O->>O: register schemas into TOOL_SCHEMAS

    Note over O: per tool_call from LLM
    O->>M: GET /sse (reconnect)
    O->>M: call_tool(name, args)
    M-->>O: result content
    O->>O: inject result → follow-up LLM call
```

### Default MCP server: `fetch-mcp`

Included in the stack at `mcp-servers/fetch/`. Provides one tool:

| Tool | Description |
|------|-------------|
| `fetch_url(url, max_chars?)` | Fetch the body of any public URL (weather, APIs, docs) |

Example prompt that uses it:
```
"What's the weather in Stockholm right now?"
→ model calls fetch_url("https://wttr.in/Stockholm?format=3")
→ returns "Stockholm: ⛅  +18°C"
```

### Adding Home Assistant MCP (built-in since HA 2024.11)

Home Assistant ships a native MCP server. Point the butler at it with:

```bash
# .env
HA_URL=http://homeassistant.local:8123
HA_TOKEN=<long-lived-access-token>

# Add to MCP_SERVERS (comma-separated, no spaces)
MCP_SERVERS=http://fetch-mcp:8400,http://homeassistant.local:8123/api/mcp_server
MCP_TOKENS=,<same-long-lived-access-token>   # first entry blank = fetch-mcp needs no auth
```

The orchestrator will automatically discover all entities HA exposes as MCP tools (call_service, get_state, etc.) and make them available to the model.

### Adding any other MCP server

1. Run the server (Docker/Podman or `npx`) so it's reachable from the orchestrator container.
2. Append its SSE base-URL to `MCP_SERVERS` in `.env`.
3. If it requires a Bearer token, append it to `MCP_TOKENS` (keep position aligned).
4. `podman-compose up --build -d orchestrator` to reconnect.

Popular community servers:

| Server | npm package | What it adds |
|--------|------------|--------------|
| Filesystem | `@modelcontextprotocol/server-filesystem` | Read/write local files |
| Brave Search | `@modelcontextprotocol/server-brave-search` | Web search (needs Brave API key) |
| SQLite | `@modelcontextprotocol/server-sqlite` | Direct SQL on any `.db` file |
| Memory | `@modelcontextprotocol/server-memory` | Persistent key-value memory graph |

Run them as containers (example for `server-filesystem`):
```bash
podman run -d --name mcp-fs \
  -v ./data/home-docs:/docs:ro \
  -p 8401:8401 \
  node:22-slim \
  npx -y @modelcontextprotocol/server-filesystem /docs --sse-port 8401
# then add http://mcp-fs:8401 to MCP_SERVERS in .env
```

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

## Build Roadmap

```mermaid
gantt
    title AI Home Butler — Phase Roadmap
    dateFormat  YYYY-MM-DD
    section Core
    Phase 0 · Foundation (/ask, echo tool)     :done,    p0, 2025-01-01, 30d
    Phase 1 · Memory & Tools (ChromaDB, portfolio, market price) :done, p1, after p0, 45d
    Phase 3 · Audio (Whisper STT + Piper TTS, WebSocket)        :done, p3, after p1, 30d
    Phase 4 · RAG (home-docs knowledge base)   :done,    p4, after p3, 30d
    section Smart Home
    Phase 4b · Smart Home (HA tools via MQTT)  :active,  p4b, 2025-05-01, 60d
    Phase 5 · Edge Devices (ESP32 orb + P4 panel) :        p5, after p4b, 60d
    section Vision & Robotics
    Phase 6 · Frigate NVR + face context       :        p6, after p5, 45d
    Phase 7 · UGV Rover (ROS 2 + MQTT)         :        p7, after p6, 90d
```

| Phase | Status | Goal |
|-------|--------|------|
| 0 — Foundation | ✓ Done | `/ask` endpoint, one tool, smoke test |
| 1 — Memory & Tools | ✓ Done | ChromaDB memory, portfolio + market price tools, cost tracking |
| 3 — Audio | ✅ Done | Whisper STT + Piper TTS over WebSocket |
| 4 — RAG | ✅ Done | Home knowledge base, auto-indexes `data/home-docs/` |
| 4b — Smart Home | 🔧 Active | Home Assistant tools via MQTT (develop on WSL) |
| 5 — Edge Devices | Planned | ESP32 voice orb + P4 dashboard (point at WSL IP during dev) |
| 6 — Vision | Planned | Frigate NVR, face-detect context piped via MQTT |
| 7 — Robotics | Planned | UGV Rover (ROS 2), mobile speaker + camera |
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

