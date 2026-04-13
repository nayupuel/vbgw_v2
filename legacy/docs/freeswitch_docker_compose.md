# FreeSWITCH Docker Compose Deployment Guide

> **Version**: v1.0.0 | 2026-04-07 | DevOps  
> **Parent**: `freeswitch_migration_v2.md`

---

## 1. Directory Structure

```
vbgw-freeswitch/
├── docker-compose.yml              # Main composition
├── docker-compose.override.yml     # Dev overrides (optional)
├── docker-compose.prod.yml         # Production overrides
├── .env                            # Environment variables (gitignored)
├── .env.example                    # Template
│
├── config/
│   └── freeswitch/
│       ├── vars.xml                # Environment variable mapping
│       ├── sip_profiles/
│       │   └── internal.xml        # Sofia SIP profile
│       ├── dialplan/
│       │   └── default.xml         # Inbound call handler
│       ├── autoload_configs/
│       │   └── event_socket.conf.xml  # ESL configuration
│       └── tls/                    # TLS certificates (prod)
│           ├── agent.pem
│           ├── cafile.pem
│           └── wss.pem
│
├── orchestrator/
│   ├── Dockerfile
│   ├── cmd/main.go
│   └── internal/...
│
├── bridge/
│   ├── Dockerfile
│   ├── cmd/main.go
│   └── internal/...
│
├── models/
│   └── silero_vad.onnx             # VAD model (from C++ project)
│
├── recordings/                     # Call recordings volume
│
└── logs/                           # Log files volume
```

---

## 2. Docker Compose (Development)

```yaml
# docker-compose.yml
version: "3.9"

services:
  # ===== Tier 1: FreeSWITCH Media Engine =====
  freeswitch:
    image: signalwire/freeswitch:1.10
    container_name: vbgw-freeswitch
    restart: unless-stopped
    network_mode: host                # Required for RTP UDP port range
    volumes:
      - ./config/freeswitch:/etc/freeswitch:ro
      - ./recordings:/recordings
      - ./logs/freeswitch:/var/log/freeswitch
    environment:
      # SIP
      - SIP_PORT=${SIP_PORT:-5060}
      - SIP_TLS_PORT=${SIP_TLS_PORT:-5061}
      - SIP_TLS_ONLY=${SIP_TLS_ONLY:-false}
      - SIP_TLS_CERT_DIR=${SIP_TLS_CERT_DIR:-/etc/freeswitch/tls}
      - SRTP_MODE=${SRTP_MODE:-optional}
      # NAT
      - EXTERNAL_RTP_IP=${EXTERNAL_RTP_IP:-auto-nat}
      - EXTERNAL_SIP_IP=${EXTERNAL_SIP_IP:-auto-nat}
      - STUN_ENABLED=${STUN_ENABLED:-true}
      # Session
      - PRACK_ENABLED=${PRACK_ENABLED:-true}
      - SESSION_TIMER_ENABLED=${SESSION_TIMER_ENABLED:-true}
      - SESSION_TIMEOUT=${SESSION_TIMEOUT:-1800}
      - MIN_SE=${MIN_SE:-90}
      - ANSWER_DELAY_MS=${ANSWER_DELAY_MS:-200}
      # RTP
      - RTP_PORT_MIN=${RTP_PORT_MIN:-16384}
      - RTP_PORT_MAX=${RTP_PORT_MAX:-16584}
      # Jitter Buffer
      - JB_INIT_MS=${JB_INIT_MS:-100}
      - JB_MIN_MS=${JB_MIN_MS:-60}
      - JB_MAX_MS=${JB_MAX_MS:-500}
      # PBX
      - PBX_HOST=${PBX_HOST:-}
      - PBX_USERNAME=${PBX_USERNAME:-}
      - PBX_PASSWORD=${PBX_PASSWORD:-}
      - PBX_REGISTER=${PBX_REGISTER:-false}
      # Bridge
      - BRIDGE_HOST=${BRIDGE_HOST:-127.0.0.1}
      - BRIDGE_WS_PORT=${BRIDGE_WS_PORT:-8090}
      # ESL
      - ESL_PASSWORD=${ESL_PASSWORD:-ClueCon}
    healthcheck:
      test: ["CMD", "fs_cli", "-x", "status"]
      interval: 10s
      timeout: 5s
      retries: 3

  # ===== Tier 2: Go Orchestrator =====
  orchestrator:
    build:
      context: ./orchestrator
      dockerfile: Dockerfile
    container_name: vbgw-orchestrator
    restart: unless-stopped
    network_mode: host                # Shares network with FS for ESL loopback
    volumes:
      - ./recordings:/recordings
      - ./logs/orchestrator:/var/log/orchestrator
    environment:
      # ESL
      - ESL_HOST=${ESL_HOST:-127.0.0.1}
      - ESL_PORT=${ESL_PORT:-8021}
      - ESL_PASSWORD=${ESL_PASSWORD:-ClueCon}
      # Bridge
      - BRIDGE_HOST=${BRIDGE_HOST:-127.0.0.1}
      - BRIDGE_INTERNAL_PORT=${BRIDGE_INTERNAL_PORT:-8091}
      # HTTP API
      - HTTP_PORT=${HTTP_PORT:-8080}
      - ADMIN_API_KEY=${ADMIN_API_KEY:-changeme-admin-key}
      - RATE_LIMIT_RPS=${RATE_LIMIT_RPS:-20}
      - RATE_LIMIT_BURST=${RATE_LIMIT_BURST:-40}
      # Session
      - MAX_SESSIONS=${MAX_SESSIONS:-100}
      # Recording
      - RECORDING_ENABLE=${RECORDING_ENABLE:-false}
      - RECORDING_DIR=${RECORDING_DIR:-/recordings}
      - RECORDING_MAX_DAYS=${RECORDING_MAX_DAYS:-30}
      - RECORDING_MAX_MB=${RECORDING_MAX_MB:-1024}
      # Runtime
      - RUNTIME_PROFILE=${RUNTIME_PROFILE:-dev}
      - LOG_LEVEL=${LOG_LEVEL:-info}
    healthcheck:
      test: ["CMD", "wget", "-q", "-O-", "http://127.0.0.1:${HTTP_PORT:-8080}/live"]
      interval: 10s
      timeout: 5s
      retries: 3
    depends_on:
      freeswitch:
        condition: service_healthy

  # ===== Tier 3: WebSocket Bridge =====
  bridge:
    build:
      context: ./bridge
      dockerfile: Dockerfile
    container_name: vbgw-bridge
    restart: unless-stopped
    network_mode: host                # Loopback access for FS + Orchestrator
    volumes:
      - ./models:/models:ro
      - ./logs/bridge:/var/log/bridge
    environment:
      # WebSocket (from FS mod_audio_fork)
      - WS_PORT=${BRIDGE_WS_PORT:-8090}
      # Internal HTTP (from Orchestrator)
      - INTERNAL_PORT=${BRIDGE_INTERNAL_PORT:-8091}
      # AI Engine (gRPC)
      - AI_GRPC_ADDR=${AI_GRPC_ADDR:-127.0.0.1:50051}
      - AI_GRPC_TLS=${AI_GRPC_TLS:-false}
      - AI_GRPC_CA_CERT=${AI_GRPC_CA_CERT:-}
      - AI_GRPC_CLIENT_CERT=${AI_GRPC_CLIENT_CERT:-}
      - AI_GRPC_CLIENT_KEY=${AI_GRPC_CLIENT_KEY:-}
      # VAD
      - ONNX_MODEL_PATH=${ONNX_MODEL_PATH:-/models/silero_vad.onnx}
      # gRPC Retry
      - GRPC_MAX_RETRIES=${GRPC_MAX_RETRIES:-5}
      - GRPC_MAX_BACKOFF_MS=${GRPC_MAX_BACKOFF_MS:-4000}
      - GRPC_STREAM_DEADLINE_SECS=${GRPC_STREAM_DEADLINE_SECS:-86400}
      # Orchestrator (for barge-in callback)
      - ORCHESTRATOR_URL=http://127.0.0.1:${HTTP_PORT:-8080}
      # Logging
      - LOG_LEVEL=${LOG_LEVEL:-info}
    healthcheck:
      test: ["CMD", "wget", "-q", "-O-", "http://127.0.0.1:${BRIDGE_INTERNAL_PORT:-8091}/internal/health"]
      interval: 10s
      timeout: 5s
      retries: 3
    depends_on:
      freeswitch:
        condition: service_healthy
```

---

## 3. Production Overrides

```yaml
# docker-compose.prod.yml
version: "3.9"

services:
  freeswitch:
    environment:
      - SIP_TLS_ONLY=true
      - SRTP_MODE=mandatory
    logging:
      driver: json-file
      options:
        max-size: "100m"
        max-file: "5"

  orchestrator:
    environment:
      - RUNTIME_PROFILE=production
      - LOG_LEVEL=warn
    logging:
      driver: json-file
      options:
        max-size: "100m"
        max-file: "5"

  bridge:
    environment:
      - AI_GRPC_TLS=true
      - LOG_LEVEL=warn
    logging:
      driver: json-file
      options:
        max-size: "100m"
        max-file: "5"
```

**Production 실행**:
```bash
docker compose -f docker-compose.yml -f docker-compose.prod.yml up -d
```

---

## 4. Dockerfiles

### 4.1 Orchestrator Dockerfile

```dockerfile
# orchestrator/Dockerfile
FROM golang:1.22-alpine AS builder
WORKDIR /app
COPY go.mod go.sum ./
RUN go mod download
COPY . .
RUN CGO_ENABLED=0 GOOS=linux go build -o /orchestrator ./cmd/main.go

FROM alpine:3.19
RUN apk add --no-cache ca-certificates wget
COPY --from=builder /orchestrator /usr/local/bin/orchestrator
USER nobody:nobody
ENTRYPOINT ["orchestrator"]
```

### 4.2 Bridge Dockerfile

```dockerfile
# bridge/Dockerfile
FROM golang:1.22-alpine AS builder
RUN apk add --no-cache gcc musl-dev    # Required for CGo (onnxruntime)
WORKDIR /app
COPY go.mod go.sum ./
RUN go mod download
COPY . .
RUN CGO_ENABLED=1 GOOS=linux go build -o /bridge ./cmd/main.go

FROM alpine:3.19
RUN apk add --no-cache ca-certificates wget libstdc++
# ONNX Runtime shared library
COPY --from=builder /usr/local/lib/libonnxruntime.so* /usr/local/lib/
COPY --from=builder /bridge /usr/local/bin/bridge
USER nobody:nobody
ENTRYPOINT ["bridge"]
```

---

## 5. Environment Variables (.env.example)

```bash
# ===== SIP =====
SIP_PORT=5060
SIP_TLS_PORT=5061
SIP_TLS_ONLY=false
SIP_TLS_CERT_DIR=/etc/freeswitch/tls
SRTP_MODE=optional

# ===== NAT =====
EXTERNAL_RTP_IP=auto-nat
EXTERNAL_SIP_IP=auto-nat
STUN_ENABLED=true

# ===== Session =====
PRACK_ENABLED=true
SESSION_TIMER_ENABLED=true
SESSION_TIMEOUT=1800
MIN_SE=90
ANSWER_DELAY_MS=200

# ===== RTP =====
RTP_PORT_MIN=16384
RTP_PORT_MAX=16584

# ===== Jitter Buffer =====
JB_INIT_MS=100
JB_MIN_MS=60
JB_MAX_MS=500

# ===== PBX =====
PBX_HOST=
PBX_USERNAME=
PBX_PASSWORD=
PBX_REGISTER=false

# ===== ESL =====
ESL_HOST=127.0.0.1
ESL_PORT=8021
ESL_PASSWORD=ClueCon

# ===== HTTP API =====
HTTP_PORT=8080
ADMIN_API_KEY=changeme-admin-key-minimum-16-chars!
RATE_LIMIT_RPS=20
RATE_LIMIT_BURST=40

# ===== Session =====
MAX_SESSIONS=100

# ===== AI Engine =====
AI_GRPC_ADDR=127.0.0.1:50051
AI_GRPC_TLS=false

# ===== Bridge =====
BRIDGE_HOST=127.0.0.1
BRIDGE_WS_PORT=8090
BRIDGE_INTERNAL_PORT=8091

# ===== VAD =====
ONNX_MODEL_PATH=/models/silero_vad.onnx

# ===== gRPC Retry =====
GRPC_MAX_RETRIES=5
GRPC_MAX_BACKOFF_MS=4000
GRPC_STREAM_DEADLINE_SECS=86400

# ===== Recording =====
RECORDING_ENABLE=false
RECORDING_DIR=/recordings
RECORDING_MAX_DAYS=30
RECORDING_MAX_MB=1024

# ===== Runtime =====
RUNTIME_PROFILE=dev
LOG_LEVEL=info
```

---

## 6. Network & Port Summary

| Service | Port | Protocol | Direction | Purpose |
|---------|------|----------|-----------|---------|
| FreeSWITCH | 5060 | UDP/TCP | External ← PBX | SIP signaling |
| FreeSWITCH | 5061 | TCP (TLS) | External ← PBX | SIP TLS |
| FreeSWITCH | 16384-16584 | UDP | External ↔ PBX | RTP/SRTP media |
| FreeSWITCH | 8021 | TCP | Internal ← Orchestrator | ESL control |
| Orchestrator | 8080 | TCP | External ← Clients | HTTP API |
| Bridge | 8090 | TCP (WS) | Internal ← FreeSWITCH | Audio streaming |
| Bridge | 8091 | TCP | Internal ← Orchestrator | Control API |
| AI Engine | 50051 | TCP (gRPC) | Internal ← Bridge | AI streaming |

---

## 7. Operations

### 7.1 Start/Stop

```bash
# Development
docker compose up -d
docker compose down

# Production
docker compose -f docker-compose.yml -f docker-compose.prod.yml up -d
docker compose -f docker-compose.yml -f docker-compose.prod.yml down

# Graceful shutdown (preserves active calls)
docker compose exec orchestrator kill -INT 1
# Wait for Shutdown 5/5 log, then:
docker compose down
```

### 7.2 Log Viewing

```bash
# All services
docker compose logs -f

# Specific service
docker compose logs -f orchestrator

# FS CLI (interactive)
docker compose exec freeswitch fs_cli
```

### 7.3 Health Checks

```bash
# Liveness
curl http://localhost:8080/live

# Readiness
curl http://localhost:8080/ready

# Full health
curl http://localhost:8080/health

# Prometheus metrics
curl http://localhost:8080/metrics
```

### 7.4 FS CLI Diagnostics

```bash
# Inside FS container
fs_cli -x "sofia status"              # SIP profile status
fs_cli -x "sofia status profile internal"  # Registration details
fs_cli -x "show calls"                # Active calls
fs_cli -x "uuid_dump <uuid>"          # Per-call RTP stats
fs_cli -x "status"                    # System status
```
