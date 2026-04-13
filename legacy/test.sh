#!/usr/bin/env bash
# =============================================================================
# VoiceBot Gateway — 로컬 통합 테스트 러너
# 사용법: ./test.sh [mock|emulator] [null|voice]
#
#   ./test.sh mock  null   — beep 응답 서버 + 무음 SIP 콜 (빠른 연결 검증)
#   ./test.sh mock  voice  — beep 응답 서버 + 실제 마이크/스피커 (음성 체험)
#   ./test.sh emul  null   — WAV 응답 서버 + 무음 SIP 콜 (오디오 캡처 검증)
#   ./test.sh emul  voice  — WAV 응답 서버 + 실제 마이크/스피커 (전체 시나리오)
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
EMULATOR_DIR="$SCRIPT_DIR/src/emulator"
VENV="$EMULATOR_DIR/venv/bin/activate"
CONFIG="$SCRIPT_DIR/config/.env.local"
BINARY="$SCRIPT_DIR/build/vbgw"

SERVER_TYPE="${1:-mock}"   # mock | emul
AUDIO_TYPE="${2:-null}"    # null | voice

# ── 전제조건 확인 ──────────────────────────────────────────────────────────────
echo ""
echo "================================================="
echo " VoiceBot Gateway 로컬 테스트 준비"
echo "================================================="

if [[ ! -f "$BINARY" ]]; then
    echo "❌  바이너리 없음: $BINARY"
    echo "    먼저 빌드하세요: cmake --build build"
    exit 1
fi

if [[ ! -f "$VENV" ]]; then
    echo "❌  Python venv 없음: $EMULATOR_DIR/venv"
    echo "    먼저 설치하세요:"
    echo "    cd $EMULATOR_DIR && python3 -m venv venv && source venv/bin/activate && pip install -r requirements.txt"
    exit 1
fi

if [[ ! -f "$SCRIPT_DIR/models/silero_vad.onnx" ]]; then
    echo "❌  VAD 모델 없음: models/silero_vad.onnx"
    exit 1
fi

echo "✅  바이너리:   $BINARY"
echo "✅  VAD 모델:   models/silero_vad.onnx"
echo "✅  Python venv: $EMULATOR_DIR/venv"
echo ""

# ── 설정 로드 ──────────────────────────────────────────────────────────────────
set -a
# shellcheck source=/dev/null
source <(grep -v '^#' "$CONFIG" | grep -v '^$')
set +a

SIP_PORT="${SIP_PORT:-5060}"

# ── 서버 타입 결정 ─────────────────────────────────────────────────────────────
if [[ "$SERVER_TYPE" == "emul" ]]; then
    AI_SERVER_SCRIPT="emulator.py"
    SERVER_LABEL="emulator.py (WAV 응답, 오디오 캡처)"
else
    AI_SERVER_SCRIPT="mock_server.py"
    SERVER_LABEL="mock_server.py (Beep 응답)"
fi

# ── 오디오 타입 결정 ───────────────────────────────────────────────────────────
if [[ "$AUDIO_TYPE" == "voice" ]]; then
    PJSUA_AUDIO_FLAG=""
    AUDIO_LABEL="실제 마이크/스피커"
else
    PJSUA_AUDIO_FLAG="--null-audio"
    AUDIO_LABEL="Null Audio (무음, 연결 검증용)"
fi

echo "테스트 구성:"
echo "  AI 서버  : $SERVER_LABEL"
echo "  오디오   : $AUDIO_LABEL"
echo "  SIP 포트 : $SIP_PORT"
echo "  gRPC 주소: ${AI_ENGINE_ADDR:-localhost:50051}"
echo ""

# ── 기존 프로세스 정리 ─────────────────────────────────────────────────────────
echo "기존 프로세스 정리 중..."
pkill -f "mock_server.py" 2>/dev/null || true
pkill -f "emulator.py"    2>/dev/null || true
pkill -x vbgw             2>/dev/null || true
sleep 0.5

# ── Step 1: gRPC AI 목 서버 시작 ───────────────────────────────────────────────
echo "[1/3] gRPC AI 서버 시작: $AI_SERVER_SCRIPT"
(
    cd "$EMULATOR_DIR"
    source "$VENV"
    python "$AI_SERVER_SCRIPT"
) &
MOCK_PID=$!
sleep 1

if ! kill -0 "$MOCK_PID" 2>/dev/null; then
    echo "❌  AI 서버 시작 실패"
    exit 1
fi
echo "     PID=$MOCK_PID — gRPC:50051 대기 중"

# ── Step 2: vbgw 시작 ─────────────────────────────────────────────────────────
echo "[2/3] vbgw 시작..."
(
    cd "$SCRIPT_DIR"
    set -a; source <(grep -v '^#' "$CONFIG" | grep -v '^$'); set +a
    exec "$BINARY"
) &
VBGW_PID=$!
sleep 2

if ! kill -0 "$VBGW_PID" 2>/dev/null; then
    echo "❌  vbgw 시작 실패"
    kill "$MOCK_PID" 2>/dev/null || true
    exit 1
fi
echo "     PID=$VBGW_PID — SIP UDP:$SIP_PORT 대기 중"

# ── Step 3: SIP 콜 발신 ────────────────────────────────────────────────────────
echo ""
echo "[3/3] SIP 테스트 콜 발신..."
echo "     대상: sip:voicebot@127.0.0.1:$SIP_PORT"
echo ""
echo "================================================="
if [[ "$AUDIO_TYPE" == "voice" ]]; then
    echo " 🎤  실제 마이크로 말하면 VAD가 감지합니다."
    echo "     말을 멈추면 AI 서버가 응답음을 재생합니다."
    echo "     'h' + Enter → 통화 종료 / 'q' + Enter → 종료"
else
    echo " ℹ️   Null Audio 모드: 실제 음성 없이 SIP 연결만 검증"
    echo "     VAD가 is_speaking=false 를 지속 전송합니다."
    echo "     'h' + Enter → 통화 종료 / 'q' + Enter → 종료"
fi
echo "================================================="
echo ""

cleanup() {
    echo ""
    echo "테스트 종료 중..."
    kill "$VBGW_PID"  2>/dev/null || true
    kill "$MOCK_PID"  2>/dev/null || true
    wait "$VBGW_PID"  2>/dev/null || true
    wait "$MOCK_PID"  2>/dev/null || true
    echo "✅  종료 완료"
}
trap cleanup EXIT INT TERM

pjsua \
    $PJSUA_AUDIO_FLAG \
    --no-tcp \
    --local-port=15060 \
    --log-level=1 \
    --app-log-level=1 \
    "sip:voicebot@127.0.0.1:$SIP_PORT"
