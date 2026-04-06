#!/usr/bin/env bash
set -euo pipefail

# End-to-end outbound API call scenario using null-audio SIP endpoint.
#
# This script starts:
#   1) mock gRPC AI server
#   2) vbgw (local mode)
#   3) pjsua callee endpoint (auto-answer)
#   4) outbound API request to vbgw
#
# Usage:
#   ./scripts/e2e_outbound_null_audio.sh [env_file]
#
# Optional env:
#   TARGET_USER   (default: callee)
#   TARGET_PORT   (default: 15060)
#   E2E_TIMEOUT   (default: 25 seconds)
#   REQUIRE_MEDIA (default: 0; if 1, fail instead of skip on audio init limitation)
#   REQUIRE_E2E   (default: 0; if 1, fail instead of skip on env limitations)

ENV_FILE="${1:-config/.env.local}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
EMULATOR_DIR="$ROOT_DIR/src/emulator"
VENV_ACTIVATE="$EMULATOR_DIR/venv/bin/activate"

TARGET_USER="${TARGET_USER:-callee}"
TARGET_PORT="${TARGET_PORT:-15060}"
E2E_TIMEOUT="${E2E_TIMEOUT:-25}"
REQUIRE_MEDIA="${REQUIRE_MEDIA:-0}"
REQUIRE_E2E="${REQUIRE_E2E:-0}"

VBGW_LOG="$(mktemp -t vbgw_e2e_vbgw).log"
AI_LOG="$(mktemp -t vbgw_e2e_ai).log"
CALLEE_LOG="$(mktemp -t vbgw_e2e_callee).log"
RESP_BODY="$(mktemp -t vbgw_e2e_resp).json"

VBGW_PID=""
AI_PID=""
CALLEE_PID=""

cleanup() {
    set +e
    if [[ -n "$CALLEE_PID" ]] && kill -0 "$CALLEE_PID" 2>/dev/null; then
        kill -INT "$CALLEE_PID" 2>/dev/null
        wait "$CALLEE_PID" 2>/dev/null
    fi
    if [[ -n "$VBGW_PID" ]] && kill -0 "$VBGW_PID" 2>/dev/null; then
        kill -INT "$VBGW_PID" 2>/dev/null
        wait "$VBGW_PID" 2>/dev/null
    fi
    if [[ -n "$AI_PID" ]] && kill -0 "$AI_PID" 2>/dev/null; then
        kill -TERM "$AI_PID" 2>/dev/null
        wait "$AI_PID" 2>/dev/null
    fi
}
trap cleanup EXIT

skip_or_fail() {
    local reason="$1"
    local log_file="${2:-}"
    if [[ "$REQUIRE_E2E" == "1" ]]; then
        echo "ERROR: ${reason}"
        if [[ -n "$log_file" && -f "$log_file" ]]; then
            tail -n 120 "$log_file" || true
        fi
        exit 2
    fi
    echo "SKIP: ${reason}"
    if [[ -n "$log_file" && -f "$log_file" ]]; then
        tail -n 80 "$log_file" || true
    fi
    exit 0
}

echo "================================================="
echo "VBGW Outbound API E2E (Null Audio)"
echo "================================================="
echo "Env file : $ENV_FILE"
echo "Logs:"
echo "  vbgw   : $VBGW_LOG"
echo "  ai     : $AI_LOG"
echo "  callee : $CALLEE_LOG"
echo ""

if [[ ! -f "$ENV_FILE" ]]; then
    echo "ERROR: env file not found: $ENV_FILE"
    exit 1
fi

if [[ ! -x "$ROOT_DIR/build/vbgw" ]]; then
    echo "ERROR: binary not found: $ROOT_DIR/build/vbgw"
    echo "Run: cmake --build build"
    exit 1
fi

if ! command -v curl >/dev/null 2>&1; then
    echo "ERROR: curl command is required"
    exit 1
fi

if ! command -v pjsua >/dev/null 2>&1; then
    echo "SKIP: pjsua command not found on PATH"
    exit 0
fi

if [[ ! -f "$VENV_ACTIVATE" ]]; then
    echo "ERROR: emulator venv not found: $VENV_ACTIVATE"
    echo "Create with:"
    echo "  cd src/emulator && python3 -m venv venv && source venv/bin/activate && pip install -r requirements.txt"
    exit 1
fi

set -a
# shellcheck source=/dev/null
source "$ENV_FILE"
set +a

export PJSIP_NULL_AUDIO=1
export ADMIN_API_KEY="${ADMIN_API_KEY:-changeme-admin-key}"

ADMIN_URL="http://127.0.0.1:${HTTP_PORT:-8080}"
TARGET_URI="sip:${TARGET_USER}@127.0.0.1:${TARGET_PORT}"

echo "Resolved settings:"
echo "  ADMIN_URL     : $ADMIN_URL"
echo "  TARGET_URI    : $TARGET_URI"
echo "  ADMIN_API_KEY : (hidden)"
echo ""

echo "[1/5] Starting mock AI server..."
(
    cd "$EMULATOR_DIR"
    # shellcheck source=/dev/null
    source "$VENV_ACTIVATE"
    exec python mock_server.py
) >"$AI_LOG" 2>&1 &
AI_PID=$!
sleep 1
if ! kill -0 "$AI_PID" 2>/dev/null; then
    if rg -q "Failed to bind|No address added|Address already in use" "$AI_LOG"; then
        skip_or_fail "mock AI server bind unavailable in this environment" "$AI_LOG"
    fi
    echo "ERROR: mock AI server failed to start"
    tail -n 80 "$AI_LOG" || true
    exit 1
fi

echo "[2/5] Starting vbgw..."
(
    cd "$ROOT_DIR"
    set -a
    # shellcheck source=/dev/null
    source "$ENV_FILE"
    set +a
    export PJSIP_NULL_AUDIO=1
    exec ./build/vbgw
) >"$VBGW_LOG" 2>&1 &
VBGW_PID=$!

sleep 3
if ! kill -0 "$VBGW_PID" 2>/dev/null; then
    if rg -q "PJMEDIA_EAUD_INIT|Audio subsystem not initialized" "$VBGW_LOG"; then
        if [[ "$REQUIRE_MEDIA" == "1" ]]; then
            skip_or_fail "vbgw failed audio initialization in this environment" "$VBGW_LOG"
        fi
        skip_or_fail "vbgw audio subsystem unavailable in this environment (PJMEDIA_EAUD_INIT)" \
            "$VBGW_LOG"
    fi
    echo "ERROR: vbgw failed to start"
    tail -n 120 "$VBGW_LOG" || true
    exit 1
fi

echo "[3/5] Starting SIP callee (pjsua null-audio auto-answer)..."
pjsua \
    --null-audio \
    --no-tcp \
    --id "sip:${TARGET_USER}@127.0.0.1" \
    --local-port="$TARGET_PORT" \
    --auto-answer=200 \
    --max-calls=1 \
    --duration=15 \
    --log-level=3 \
    --app-log-level=3 >"$CALLEE_LOG" 2>&1 &
CALLEE_PID=$!

sleep 2
if ! kill -0 "$CALLEE_PID" 2>/dev/null; then
    if rg -q "PJMEDIA_EAUD_INIT|Audio subsystem not initialized" "$CALLEE_LOG"; then
        if [[ "$REQUIRE_MEDIA" == "1" ]]; then
            skip_or_fail "pjsua callee failed audio initialization" "$CALLEE_LOG"
        fi
        skip_or_fail "pjsua audio subsystem unavailable in this environment (PJMEDIA_EAUD_INIT)" \
            "$CALLEE_LOG"
    fi
    echo "ERROR: pjsua callee failed to start"
    tail -n 120 "$CALLEE_LOG" || true
    exit 1
fi

echo "[4/5] Triggering outbound API..."
HTTP_CODE="$(curl -sS -o "$RESP_BODY" -w "%{http_code}" \
    -X POST \
    -H "X-Admin-Key: ${ADMIN_API_KEY}" \
    -H "Content-Type: application/json" \
    "${ADMIN_URL}/api/v1/calls" \
    -d "{\"target_uri\":\"${TARGET_URI}\"}" || echo "000")"

echo "HTTP status: $HTTP_CODE"
echo "Response   : $(cat "$RESP_BODY")"

if [[ "$HTTP_CODE" != "202" ]]; then
    echo "ERROR: outbound API did not return 202"
    tail -n 120 "$VBGW_LOG" || true
    tail -n 120 "$CALLEE_LOG" || true
    exit 1
fi

CALL_ID="$(sed -n 's/.*"call_id":\([0-9]\+\).*/\1/p' "$RESP_BODY" | head -n 1)"
if [[ -z "$CALL_ID" ]]; then
    echo "ERROR: call_id not found in API response"
    exit 1
fi

echo "[5/5] Waiting for call state transitions..."
deadline=$((SECONDS + E2E_TIMEOUT))
connected=0
while [[ "$SECONDS" -lt "$deadline" ]]; do
    if rg -q "call_id=${CALL_ID}|Call-ID: ${CALL_ID}|State=CONFIRMED|Outgoing SIP call initiated" "$VBGW_LOG"; then
        connected=1
        break
    fi
    sleep 1
done

if [[ "$connected" -ne 1 ]]; then
    echo "ERROR: outbound call did not reach expected state within timeout"
    tail -n 140 "$VBGW_LOG" || true
    tail -n 140 "$CALLEE_LOG" || true
    exit 1
fi

echo ""
echo "PASS: Outbound API E2E scenario completed successfully."
