# =============================================================================
# VoiceBot Gateway (vbgw) — Multi-stage Dockerfile
# Stage 1 (builder): 빌드 의존성 포함한 컴파일 환경
# Stage 2 (runtime): 최소 런타임 이미지 (공격 표면 최소화)
# =============================================================================

# ── Stage 1: Builder ──────────────────────────────────────────────────────────
FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    git \
    pkg-config \
    libpjproject-dev \
    libgrpc++-dev \
    libprotobuf-dev \
    protobuf-compiler \
    protobuf-compiler-grpc \
    libonnxruntime-dev \
    libspeexdsp-dev \
    libssl-dev \
    libspdlog-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY . .

RUN cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --parallel

# ── Stage 2: Runtime ──────────────────────────────────────────────────────────
FROM ubuntu:22.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    libpjproject2 \
    libgrpc++1 \
    libprotobuf23 \
    libonnxruntime \
    libspeexdsp1 \
    libssl3 \
    libspdlog1 \
    curl \
    && rm -rf /var/lib/apt/lists/*

# [보안] non-root 사용자로 실행
RUN groupadd --gid 1001 vbgw && \
    useradd --uid 1001 --gid vbgw --no-create-home --shell /sbin/nologin vbgw

WORKDIR /app

COPY --from=builder /build/build/vbgw ./vbgw
COPY --from=builder /build/models ./models

# 로그 디렉토리 생성 및 권한 설정
RUN mkdir -p /var/log/vbgw && chown vbgw:vbgw /var/log/vbgw

USER vbgw

# SIP 수신 포트 (UDP)
EXPOSE 5060/udp

# RTP 미디어 포트 범위 (필요 시 조정)
EXPOSE 16000-16100/udp

# [S-4 Fix] HTTP 기반 헬스체크로 변경 — 프로세스 좀비/교착 상태도 감지
# pgrep은 프로세스 존재만 확인하여 deadlock 상태를 놓칠 수 있음
HEALTHCHECK --interval=5s --timeout=3s --start-period=10s --retries=3 \
    CMD curl -sf http://localhost:8080/live || exit 1

# [C-1 Fix] GRPC_USE_TLS 기본값 0 — 개발/테스트 전용
# ⚠️  WARNING: 운영(Production) 환경에서는 반드시 GRPC_USE_TLS=1 로 재설정하세요.
#              평문 gRPC 채널은 도청 및 중간자 공격에 취약합니다 (CWE-319).
#              docker run -e GRPC_USE_TLS=1 -e GRPC_TLS_CA_CERT=... 로 주입하세요.
ENV LOG_LEVEL=info \
    LOG_DIR=/var/log/vbgw \
    SIP_PORT=5060 \
    SIP_USE_TLS=0 \
    RTP_PORT_MIN=16000 \
    RTP_PORT_MAX=16100 \
    AI_ENGINE_ADDR=localhost:50051 \
    GRPC_USE_TLS=0 \
    GRPC_STREAM_DEADLINE_SECS=86400 \
    GRPC_MAX_RECONNECT_RETRIES=5 \
    GRPC_MAX_BACKOFF_MS=4000 \
    MAX_CONCURRENT_CALLS=100 \
    ANSWER_DELAY_MS=200 \
    TTS_BUFFER_SECS=5 \
    PJSIP_LOG_LEVEL=3 \
    SILERO_VAD_MODEL_PATH=/app/models/silero_vad.onnx

CMD ["./vbgw"]
