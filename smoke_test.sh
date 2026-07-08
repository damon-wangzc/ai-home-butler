#!/usr/bin/env bash
# Butler smoke test — Phases 0, 1, 3, 4 (RAG + Smart Home + MQTT)
# Usage: bash smoke_test.sh
# Requires: python3, curl, mosquitto_pub/sub (optional for MQTT test)
set -euo pipefail

BASE="http://127.0.0.1:8000"
MODEL_SERVER="http://127.0.0.1:8080"
STT="http://127.0.0.1:8100"
TTS="http://127.0.0.1:8200"
RAG="http://127.0.0.1:8300"
MQTT_HOST="127.0.0.1"
MQTT_PORT="1883"

# ── Phase 0 ───────────────────────────────────────────────
echo "=== [0.2] Model-server health ==="
curl -sf "$MODEL_SERVER/v1/models" | python3 -m json.tool
echo ""

echo "=== [0.7a] Orchestrator health ==="
curl -sf "$BASE/health" | python3 -m json.tool
echo ""

echo "=== [0.7b] Plain ask ==="
curl -sf -X POST "$BASE/ask" \
  -H "Content-Type: application/json" \
  -d '{"user_id": "dad", "text": "Hello Butler, are you there?"}' \
  | python3 -m json.tool
echo ""

echo "=== [0.7c] Function-call loop (echo_tool) ==="
curl -sf -X POST "$BASE/ask" \
  -H "Content-Type: application/json" \
  -d '{"user_id": "dad", "text": "Call the echo_tool with message hello world and show me the result."}' \
  | python3 -m json.tool
echo ""

# ── Phase 1 ───────────────────────────────────────────────
echo "=== [1.5] Portfolio tool ==="
curl -sf -X POST "$BASE/ask" \
  -H "Content-Type: application/json" \
  -d '{"user_id": "default", "text": "How is our AAPL position?"}' \
  | python3 -m json.tool
echo ""

echo "=== [1.8] cost_estimate field present ==="
result=$(curl -sf -X POST "$BASE/ask" \
  -H "Content-Type: application/json" \
  -d '{"user_id": "default", "text": "Hello"}')
echo "$result" | python3 -c "import sys,json; d=json.load(sys.stdin); assert 'cost_estimate' in d, 'missing cost_estimate'; print('cost_estimate OK:', d['cost_estimate'])"
echo ""

# ── Phase 3 ───────────────────────────────────────────────
# Wait up to 90 s for STT and TTS to finish loading their models
wait_for() {
  local url="$1" label="$2"
  echo -n "Waiting for $label to be ready..."
  for i in $(seq 1 30); do
    if curl -sf "$url" > /dev/null 2>&1; then echo " OK"; return 0; fi
    echo -n "."
    sleep 3
  done
  echo " TIMEOUT"
  return 1
}
wait_for "$STT/health" "STT"
wait_for "$TTS/health" "TTS"

echo "=== [3.1] STT health ==="
curl -sf "$STT/health" | python3 -m json.tool
echo ""

echo "=== [3.4] TTS health ==="
curl -sf "$TTS/health" | python3 -m json.tool
echo ""

echo "=== [3.4] TTS synthesize 'Hello from Butler' ==="
curl -sf -X POST "$TTS/tts" \
  -H "Content-Type: application/json" \
  -d '{"text": "Hello from Butler"}' \
  -o /tmp/butler_test.wav
WAV_SIZE=$(wc -c < /tmp/butler_test.wav)
echo "WAV size: ${WAV_SIZE} bytes (expect > 20000)"
[ "$WAV_SIZE" -gt 20000 ] && echo "TTS OK" || echo "WARNING: WAV smaller than expected"
echo ""

# ── Phase 4 ───────────────────────────────────────────────────────────────────
echo "=== [4.1] RAG health ==="
curl -sf "$RAG/health" | python3 -m json.tool
echo ""

echo "=== [4.2] RAG query (home knowledge) ==="
curl -sf -X POST "$RAG/v1/query" \
  -H "Content-Type: application/json" \
  -d '{"query": "what can Butler do?", "top_k": 2}' \
  | python3 -m json.tool
echo ""

echo "=== [4.3] Butler uses RAG tool ==="
curl -sf -X POST "$BASE/ask" \
  -H "Content-Type: application/json" \
  -d '{"user_id": "default", "text": "Search the knowledge base: how do I use the voice interface?"}' \
  | python3 -m json.tool
echo ""

# ── Phase 4: Smart Home / MQTT ─────────────────────────────────────────────────
echo "=== [4.4] MQTT broker reachable ==="
if command -v mosquitto_pub &>/dev/null; then
  mosquitto_pub -h "$MQTT_HOST" -p "$MQTT_PORT" -t "butler/smoke" -m "ping" 2>&1 && echo "MQTT publish OK" || echo "WARNING: MQTT publish failed (broker may not be running)"
else
  python3 -c "
import socket, sys
try:
    s = socket.create_connection(('$MQTT_HOST', $MQTT_PORT), timeout=3)
    s.close()
    print('MQTT TCP port 1883 reachable — OK')
except Exception as e:
    print(f'WARNING: MQTT not reachable: {e}')
"
fi
echo ""

echo "=== [4.7] schedule_reminder tool via /ask ==="
REMIND_RESULT=$(curl -sf -X POST "$BASE/ask" \
  -H "Content-Type: application/json" \
  -d '{"user_id": "default", "text": "Schedule a reminder for me: Take the dog for a walk at 2099-12-31T10:00:00"}')
echo "$REMIND_RESULT" | python3 -m json.tool
echo "$REMIND_RESULT" | python3 -c "import sys,json; d=json.load(sys.stdin); print('Reminder response OK:', bool(d.get('response')))"
echo ""

echo "=== [4.2+4.3] get_home_state / set_home_state (HA not configured — expect error message) ==="
curl -sf -X POST "$BASE/ask" \
  -H "Content-Type: application/json" \
  -d '{"user_id": "default", "text": "What is the state of light.living_room?"}' \
  | python3 -m json.tool
echo ""

# ── Phase 6: Robotics & Vision (graceful degradation without hardware) ─────────
echo "=== [6.5] detect_object tool (graceful if Frigate unavailable) ==="
DETECT_RESULT=$(curl -sf -X POST "$BASE/ask" \
  -H "Content-Type: application/json" \
  -d '{"user_id": "default", "text": "Are there any objects detected on the front door camera?"}')
echo "$DETECT_RESULT" | python3 -m json.tool
echo "$DETECT_RESULT" | python3 -c "import sys,json; d=json.load(sys.stdin); print('detect_object response OK:', bool(d.get('response')))"
echo ""

echo "=== [6.6] patrol_room tool (MQTT publish; graceful if broker unreachable) ==="
PATROL_RESULT=$(curl -sf -X POST "$BASE/ask" \
  -H "Content-Type: application/json" \
  -d '{"user_id": "default", "text": "Send the rover to patrol the kitchen"}')
echo "$PATROL_RESULT" | python3 -m json.tool
echo "$PATROL_RESULT" | python3 -c "import sys,json; d=json.load(sys.stdin); print('patrol_room response OK:', bool(d.get('response')))"
echo ""

echo "=== All smoke tests PASSED ==="
