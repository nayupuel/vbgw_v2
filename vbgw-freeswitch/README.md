# VoiceBot Gateway (VBGW) — FreeSWITCH Edition v3.1.0

AI 콜봇 인프라를 위한 3-Tier 실시간 음성 게이트웨이입니다.
PBX/SBC에서 SIP 전화를 수신하여 AI 엔진(STT/TTS/NLU)과 실시간 음성 스트리밍을 중계합니다.

---

## 목차

1. [아키텍처 개요](#1-아키텍처-개요)
2. [사전 준비사항](#2-사전-준비사항)
3. [빠른 시작 (Quick Start)](#3-빠른-시작-quick-start)
4. [환경변수 설정 상세](#4-환경변수-설정-상세)
5. [프로덕션 배포](#5-프로덕션-배포)
6. [API 레퍼런스](#6-api-레퍼런스)
7. [모니터링 및 헬스체크](#7-모니터링-및-헬스체크)
8. [테스트](#8-테스트)
9. [트러블슈팅](#9-트러블슈팅)
10. [디렉토리 구조](#10-디렉토리-구조)
11. [보안 가이드](#11-보안-가이드)
12. [라이선스](#12-라이선스)

---

## 1. 아키텍처 개요

VBGW는 3개의 독립 서비스로 구성됩니다:

```
PBX / SBC
    |
    | SIP INVITE (G.711)
    v
+-----------------------+
| Tier 1: FreeSWITCH    |  <-- SIP 시그널링 + RTP 미디어
| (signalwire/1.10.12)  |      mod_sofia, mod_audio_fork, mod_event_socket
+-----------+-----------+      mod_acl, mod_conference
            |
            | ESL (TCP 8021)            WebSocket (8090)
            v                           |
+-----------------------+               |
| Tier 2: Orchestrator  |  <-- 콜 제어, 세션 관리, REST API
| (Go, port 8080)       |      ESL 명령, IVR FSM, CDR
+-----------+-----------+
            |
            | HTTP (8091)
            v
+-----------------------+       gRPC Bi-dir Stream
| Tier 3: Bridge        |  --> AI Engine (STT/TTS/NLU)
| (Go, port 8090/8091)  |      Silero VAD, 오디오 파이프라인
+-----------------------+       (port 50051)
```

### 각 서비스 역할

| 서비스 | 역할 | 포트 |
|--------|------|------|
| **FreeSWITCH** | SIP 시그널링, RTP 미디어 수신/송신, 코덱 처리, DTMF | 5060 (SIP), 5061 (TLS), 16384-16584 (RTP) |
| **Orchestrator** | REST API, ESL 명령 제어, 세션 관리, IVR 상태머신, CDR 로깅 | 8080 (HTTP API) |
| **Bridge** | WebSocket 오디오 수신, VAD 추론, gRPC AI 스트리밍, TTS 재생 | 8090 (WS), 8091 (Internal) |

### 데이터 흐름

```
1. PBX가 SIP INVITE 전송 → FreeSWITCH가 수신 + 응답
2. FreeSWITCH가 mod_audio_fork로 RTP 오디오를 WebSocket으로 전달
3. Bridge가 오디오 수신 → Silero VAD로 음성 구간 감지
4. 음성 구간의 오디오를 gRPC로 AI 엔진에 전송
5. AI 엔진이 STT 결과 또는 TTS 오디오를 반환
6. Bridge가 TTS 오디오를 FreeSWITCH에 전달 → RTP로 상대방에게 송출
```

---

## 2. 사전 준비사항

### 필수 소프트웨어

| 소프트웨어 | 최소 버전 | 확인 명령어 |
|-----------|----------|------------|
| Docker | 24.0+ | `docker --version` |
| Docker Compose | 2.20+ | `docker compose version` |
| Git | 2.30+ | `git --version` |

### 선택 (개발/테스트용)

| 소프트웨어 | 용도 | 설치 |
|-----------|------|------|
| Go | Orchestrator/Bridge 로컬 빌드 | `brew install go` (1.21+) |
| SIPp | SIP 부하 테스트 | `brew install sipp` |
| jq | JSON 응답 파싱 | `brew install jq` |
| curl | API 테스트 | 대부분 OS 기본 설치 |

### 하드웨어 권장사양

| 항목 | 개발 | 프로덕션 (100 동시호) |
|------|------|---------------------|
| CPU | 2 코어 | 4 코어 이상 |
| RAM | 2 GB | 4 GB 이상 |
| 디스크 | 10 GB | 50 GB (녹음 포함) |
| 네트워크 | 로컬 | 전용선 또는 QoS 보장 |

---

## 3. 빠른 시작 (Quick Start)

### Step 1: 저장소 클론

```bash
git clone https://github.com/nayupuel/vbgw_v2.git
cd vbgw_v2/vbgw-freeswitch
```

### Step 2: 환경변수 파일 생성

```bash
cp .env.example .env
```

### Step 3: 필수 시크릿 설정

`.env` 파일을 열어 아래 3개 값을 반드시 변경합니다:

```bash
# .env 파일 편집
vi .env   # 또는 nano .env, code .env 등 원하는 에디터 사용
```

변경할 항목:

```bash
# 1) ESL 비밀번호 — FreeSWITCH와 Orchestrator가 통신할 때 사용
#    반드시 변경! 기본값 없이 서비스가 시작되지 않습니다.
ESL_PASSWORD=my-strong-esl-password-here

# 2) Admin API Key — REST API 인증에 사용
#    반드시 변경! 기본값 없이 서비스가 시작되지 않습니다.
ADMIN_API_KEY=my-strong-api-key-here

# 3) AI 엔진 주소 — STT/TTS/NLU 서버의 gRPC 주소
AI_GRPC_ADDR=127.0.0.1:50051
```

강력한 랜덤 비밀번호 생성법:

```bash
# ESL 비밀번호 (16바이트 hex)
openssl rand -hex 16
# 출력 예: a1b2c3d4e5f6789012345678abcdef01

# Admin API Key (32바이트 hex)
openssl rand -hex 32
# 출력 예: a1b2c3d4...64자리 hex 문자열
```

### Step 4: 필수 디렉토리 생성

```bash
mkdir -p recordings logs/freeswitch logs/orchestrator logs/bridge models
```

### Step 5: VAD 모델 배치

Silero VAD v4 모델 파일을 `models/` 폴더에 배치합니다:

```bash
# 이미 models/silero_vad.onnx 파일이 있다면 이 단계는 건너뜁니다
ls models/silero_vad.onnx
```

> **참고**: VAD 모델 파일이 없어도 Bridge는 energy-based VAD 폴백으로 동작합니다.
> 다만 정확도가 떨어지므로 프로덕션에서는 ONNX 모델 사용을 권장합니다.

### Step 6: 서비스 시작

```bash
docker compose up -d
```

처음 실행 시 Orchestrator와 Bridge 이미지 빌드에 2-5분 소요됩니다.

### Step 7: 서비스 상태 확인

```bash
# 전체 컨테이너 상태 확인
docker compose ps

# 기대 결과: 3개 서비스 모두 "healthy" 상태
# vbgw-freeswitch    healthy
# vbgw-orchestrator  healthy
# vbgw-bridge        healthy
```

개별 헬스체크:

```bash
# FreeSWITCH 상태
docker exec vbgw-freeswitch fs_cli -x "sofia status"

# Orchestrator liveness
curl -s http://127.0.0.1:8080/live
# 기대 결과: {"status":"ok"}

# Orchestrator readiness (ESL + Bridge 연결 확인)
curl -s http://127.0.0.1:8080/ready
# 기대 결과: {"status":"ready"}

# Bridge 상태
curl -s http://127.0.0.1:8091/internal/health
# 기대 결과: {"status":"ok","active_sessions":0}
```

### Step 8: API 동작 확인

```bash
# 상세 헬스 정보 (인증 필요)
curl -s -H "Authorization: Bearer YOUR_ADMIN_API_KEY" \
  http://127.0.0.1:8080/health | jq .

# 기대 결과:
# {
#   "status": "healthy",
#   "esl_connected": true,
#   "bridge_healthy": true,
#   "active_calls": 0,
#   "max_sessions": 100
# }
```

---

## 4. 환경변수 설정 상세

### 필수 설정 (반드시 변경)

| 변수 | 설명 | 예시 |
|------|------|------|
| `ESL_PASSWORD` | FreeSWITCH ESL 인증 비밀번호 | `openssl rand -hex 16` |
| `ADMIN_API_KEY` | REST API 인증 키 (최소 32자) | `openssl rand -hex 32` |
| `AI_GRPC_ADDR` | AI 엔진 gRPC 주소 | `10.0.1.50:50051` |

### SIP 설정

| 변수 | 기본값 | 설명 |
|------|--------|------|
| `SIP_PORT` | `5060` | SIP UDP/TCP 수신 포트 |
| `SIP_TLS_PORT` | `5061` | SIP TLS 수신 포트 |
| `SIP_TLS_ONLY` | `false` | `true` 설정 시 TLS만 허용 |
| `SIP_TLS_CERT_DIR` | `/etc/freeswitch/tls` | TLS 인증서 디렉토리 |
| `SRTP_MODE` | `optional` | `optional` / `mandatory` / `forbidden` |

### PBX 연동 설정

| 변수 | 기본값 | 설명 |
|------|--------|------|
| `PBX_HOST` | (빈값) | 기본 PBX/SBC IP 주소 |
| `PBX_USERNAME` | (빈값) | SIP REGISTER 사용자명 |
| `PBX_PASSWORD` | (빈값) | SIP REGISTER 비밀번호 |
| `PBX_REGISTER` | `false` | PBX에 SIP 등록 여부 |
| `PBX_STANDBY_HOST` | (빈값) | 예비 PBX IP (failover) |

### RTP / 코덱 설정

| 변수 | 기본값 | 설명 |
|------|--------|------|
| `RTP_PORT_MIN` | `16384` | RTP 포트 범위 시작 |
| `RTP_PORT_MAX` | `16584` | RTP 포트 범위 끝 (200포트 = 100 동시호) |
| `INBOUND_CODEC_PREFS` | `PCMU,PCMA,G722,opus` | 인바운드 코덱 우선순위 |
| `JB_INIT_MS` | `60` | Jitter Buffer 초기값 (ms) |
| `JB_MIN_MS` | `20` | Jitter Buffer 최소값 (ms) |
| `JB_MAX_MS` | `200` | Jitter Buffer 최대값 (ms) |

### 보안 설정

| 변수 | 기본값 | 설명 |
|------|--------|------|
| `PBX_ACL_IP1` | `0.0.0.0/0` | 허용할 PBX IP 1 (프로덕션: CIDR 지정!) |
| `PBX_ACL_IP2` | `0.0.0.0/0` | 허용할 PBX IP 2 |
| `AUDIO_FORK_SCHEME` | `ws` | 오디오 포크 프로토콜 (`ws` / `wss`) |
| `RATE_LIMIT_RPS` | `20` | API 초당 요청 제한 |
| `RATE_LIMIT_BURST` | `40` | API 버스트 허용량 |

### 세션 / 녹음 설정

| 변수 | 기본값 | 설명 |
|------|--------|------|
| `MAX_SESSIONS` | `100` | 최대 동시 통화 수 |
| `SESSION_TIMEOUT` | `1800` | 통화 최대 시간 (초, 0=무제한) |
| `RECORDING_ENABLE` | `false` | 녹음 활성화 |
| `RECORDING_DIR` | `/recordings` | 녹음 파일 저장 경로 |
| `RECORDING_MAX_DAYS` | `30` | 녹음 보관 일수 |

### gRPC / Bridge 설정

| 변수 | 기본값 | 설명 |
|------|--------|------|
| `GRPC_MAX_RETRIES` | `5` | gRPC 재연결 최대 횟수 |
| `GRPC_MAX_BACKOFF_MS` | `4000` | 재연결 최대 대기 (ms) |
| `GRPC_STREAM_DEADLINE_SECS` | `7200` | gRPC 스트림 타임아웃 (초) |
| `BRIDGE_WS_PORT` | `8090` | WebSocket 수신 포트 |
| `BRIDGE_INTERNAL_PORT` | `8091` | Bridge 내부 API 포트 |

---

## 5. 프로덕션 배포

### Step 1: 프로덕션 환경변수 설정

```bash
cp .env.example .env
vi .env
```

프로덕션에서 반드시 변경할 항목:

```bash
# 보안: 강력한 비밀번호
ESL_PASSWORD=$(openssl rand -hex 16)
ADMIN_API_KEY=$(openssl rand -hex 32)

# 보안: SIP ACL — PBX IP만 허용
PBX_ACL_IP1=10.10.1.100/32
PBX_ACL_IP2=10.10.1.101/32

# 보안: TLS/SRTP 강제
SIP_TLS_ONLY=true
SRTP_MODE=mandatory
AUDIO_FORK_SCHEME=wss

# 런타임: 프로덕션 모드 (pprof 비활성화)
RUNTIME_PROFILE=production

# PBX 연동
PBX_HOST=10.10.1.100
PBX_USERNAME=vbgw
PBX_PASSWORD=your-sip-password
PBX_REGISTER=true
```

### Step 2: TLS 인증서 배치

```bash
mkdir -p config/freeswitch/tls

# 인증서 파일 복사
cp /path/to/your/cert.pem config/freeswitch/tls/tls.crt
cp /path/to/your/key.pem  config/freeswitch/tls/tls.key
cp /path/to/your/ca.pem   config/freeswitch/tls/ca-bundle.crt

# 권한 설정
chmod 644 config/freeswitch/tls/tls.crt
chmod 600 config/freeswitch/tls/tls.key
```

### Step 3: 프로덕션 오버레이로 시작

```bash
docker compose -f docker-compose.yml -f docker-compose.prod.yml up -d
```

`docker-compose.prod.yml`은 아래 항목을 추가합니다:
- FreeSWITCH 재시작 정책 (`restart: on-failure:5`)
- `AUDIO_FORK_SCHEME=wss` (TLS 오디오 포크)
- 로그 로테이션, 리소스 제한

### Step 4: 프로덕션 사전 검증

```bash
bash scripts/validate_prod.sh
```

이 스크립트는 다음을 검증합니다:
- ESL 비밀번호가 기본값이 아닌지 확인
- Admin API Key가 32자 이상인지 확인
- TLS 인증서 존재 및 만료일 확인
- SRTP 설정 확인
- RTP 포트 범위 확인
- 디스크 여유 공간 확인

### Step 5: Blue/Green 전환 (선택)

기존 시스템에서 마이그레이션하는 경우:

```bash
# 10% → 25% → 50% → 100% 단계적 전환
# SLO 99.9% 미달 시 자동 롤백
bash scripts/cutover.sh
```

---

## 6. API 레퍼런스

모든 API 요청에는 인증 헤더가 필요합니다 (`/live`, `/ready` 제외):

```
Authorization: Bearer YOUR_ADMIN_API_KEY
```

### 헬스체크

| 메서드 | 경로 | 인증 | 설명 |
|--------|------|------|------|
| GET | `/live` | 불필요 | Liveness probe (K8s용) |
| GET | `/ready` | 불필요 | Readiness probe (ESL+Bridge 연결 확인) |
| GET | `/health` | 필요 | 상세 상태 (활성 콜 수, 컴포넌트 상태) |
| GET | `/metrics` | 필요 | Prometheus 메트릭 |

### 콜 관리

#### POST /api/v1/calls — 아웃바운드 콜 생성

```bash
curl -X POST http://127.0.0.1:8080/api/v1/calls \
  -H "Authorization: Bearer YOUR_API_KEY" \
  -H "Content-Type: application/json" \
  -d '{"target_uri": "1001@10.10.1.100"}'
```

응답 (201 Created):
```json
{
  "call_id": "abc-123-def",
  "fs_uuid": "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx",
  "status": "initiating"
}
```

에러 응답:
- `400` — target_uri 누락 또는 잘못된 형식
- `503` — 최대 세션 수 초과 (MAX_SESSIONS)

#### GET /api/v1/calls/{id}/stats — 콜 상태 조회

```bash
curl http://127.0.0.1:8080/api/v1/calls/abc-123-def/stats \
  -H "Authorization: Bearer YOUR_API_KEY"
```

응답 (200 OK):
```json
{
  "Caller-Caller-ID-Number": "01012345678",
  "Channel-State": "CS_EXCHANGE_MEDIA",
  "variable_duration": "120",
  ...
}
```

### 콜 제어

#### POST /api/v1/calls/{id}/dtmf — DTMF 전송

```bash
curl -X POST http://127.0.0.1:8080/api/v1/calls/abc-123-def/dtmf \
  -H "Authorization: Bearer YOUR_API_KEY" \
  -H "Content-Type: application/json" \
  -d '{"digits": "1234#"}'
```

- digits: `0-9`, `*`, `#`, `A-D` (최대 20자)

#### POST /api/v1/calls/{id}/transfer — 콜 전환

```bash
curl -X POST http://127.0.0.1:8080/api/v1/calls/abc-123-def/transfer \
  -H "Authorization: Bearer YOUR_API_KEY" \
  -H "Content-Type: application/json" \
  -d '{"target": "1000@pbx"}'
```

#### POST /api/v1/calls/{id}/attended-transfer — 안내 전환

```bash
curl -X POST http://127.0.0.1:8080/api/v1/calls/abc-123-def/attended-transfer \
  -H "Authorization: Bearer YOUR_API_KEY" \
  -H "Content-Type: application/json" \
  -d '{"target": "1000@pbx"}'
```

차이점: 일반 전환은 즉시 연결을 넘기지만, 안내 전환은 상담원에게 먼저 연결한 후 통화를 이전합니다.

#### POST /api/v1/calls/{id}/record/start — 녹음 시작

```bash
curl -X POST http://127.0.0.1:8080/api/v1/calls/abc-123-def/record/start \
  -H "Authorization: Bearer YOUR_API_KEY"
```

응답: `{"status":"recording","path":"/recordings/abc-123-def.wav"}`

#### POST /api/v1/calls/{id}/record/stop — 녹음 중지

```bash
curl -X POST http://127.0.0.1:8080/api/v1/calls/abc-123-def/record/stop \
  -H "Authorization: Bearer YOUR_API_KEY"
```

#### POST /api/v1/calls/{id}/eavesdrop — 감독자 감청

```bash
curl -X POST http://127.0.0.1:8080/api/v1/calls/abc-123-def/eavesdrop \
  -H "Authorization: Bearer YOUR_API_KEY" \
  -H "Content-Type: application/json" \
  -d '{"supervisor_uuid": "supervisor-fs-uuid"}'
```

감독자가 상담원과 고객 간 통화를 모니터링합니다 (감독자 음성은 전달되지 않음).

#### POST /api/v1/calls/bridge — 두 콜 브릿지

```bash
curl -X POST http://127.0.0.1:8080/api/v1/calls/bridge \
  -H "Authorization: Bearer YOUR_API_KEY" \
  -H "Content-Type: application/json" \
  -d '{"call_id_1": "abc-123", "call_id_2": "def-456"}'
```

#### POST /api/v1/calls/unbridge — 브릿지 해제

```bash
curl -X POST http://127.0.0.1:8080/api/v1/calls/unbridge \
  -H "Authorization: Bearer YOUR_API_KEY" \
  -H "Content-Type: application/json" \
  -d '{"call_id_1": "abc-123", "call_id_2": "def-456"}'
```

---

## 7. 모니터링 및 헬스체크

### Prometheus 메트릭

```bash
curl -H "Authorization: Bearer YOUR_API_KEY" http://127.0.0.1:8080/metrics
```

주요 메트릭:

| 메트릭 | 타입 | 설명 |
|--------|------|------|
| `vbgw_active_calls` | Gauge | 현재 활성 통화 수 |
| `vbgw_total_calls` | Counter | 누적 통화 수 |
| `vbgw_call_setup_duration_seconds` | Histogram | 콜 셋업 지연 (PDD) |
| `vbgw_call_duration_seconds` | Histogram | 통화 시간 분포 |
| `vbgw_call_hangup_total` | Counter | 종료 원인별 카운터 |
| `vbgw_esl_connected` | Gauge | ESL 연결 상태 (1/0) |
| `vbgw_bridge_connected` | Gauge | Bridge 연결 상태 (1/0) |
| `vbgw_api_latency_seconds` | Histogram | API 응답 시간 |
| `vbgw_api_requests_total` | Counter | API 요청 수 (코드별) |
| `vbgw_sip_registration_alarm` | Gauge | PBX 등록 상태 (1=정상) |

### Grafana 대시보드 쿼리

`docs/grafana_queries.md`에 12개의 PromQL 쿼리와 알람 조건이 정의되어 있습니다:

```bash
cat docs/grafana_queries.md
```

### 서비스별 로그

```bash
# FreeSWITCH 로그
tail -f logs/freeswitch/freeswitch.log

# Orchestrator 로그
docker logs -f vbgw-orchestrator

# Bridge 로그
docker logs -f vbgw-bridge
```

---

## 8. 테스트

### 유닛 테스트 실행

```bash
# Orchestrator 테스트 (89개)
cd orchestrator && CGO_ENABLED=0 go test -count=1 ./...

# Bridge 테스트 (44개)
cd bridge && CGO_ENABLED=0 go test -count=1 ./...
```

테스트 커버리지 측정:

```bash
cd orchestrator && go test -coverprofile=coverage.out ./... && go tool cover -func=coverage.out
cd bridge && go test -coverprofile=coverage.out ./... && go tool cover -func=coverage.out
```

### E2E 테스트 (SIPp 필요)

```bash
# 전체 TC-01 ~ TC-13 테스트 스위트
bash scripts/run_tc_all.sh
```

테스트 시나리오:

| TC | 설명 |
|----|------|
| TC-01 ~ TC-08 | 기본 SIP 콜 플로우, 코덱, DTMF, 녹음 |
| TC-09 | 10 동시 콜 부하 + 용량 초과 거부 확인 |
| TC-10 | DTMF IVR 상태 전이 (1->AI_CHAT, 0->TRANSFER, #->DISCONNECT) |
| TC-11 | 녹음 시작/중지 + 파일 생성 확인 |
| TC-12 | Graceful Shutdown 중 BYE 전송 확인 |
| TC-13 | Prometheus 메트릭 완전성 검증 |

### 부하 테스트 (SIPp 필요)

```bash
# 100 동시콜, 5분 유지, SLO 모니터링 포함
bash scripts/run_load_test.sh
```

---

## 9. 트러블슈팅

자세한 증상별 진단 매트릭스는 `docs/troubleshooting.md`를 참조하세요.

### 자주 발생하는 문제

#### 서비스가 시작되지 않음

```bash
# 1. 환경변수 확인
docker compose config | grep ESL_PASSWORD
# ESL_PASSWORD가 비어 있으면 .env에 설정 필요

# 2. 빌드 로그 확인
docker compose build --no-cache orchestrator
docker compose build --no-cache bridge

# 3. 의존성 순서 확인 (FS → Orchestrator → Bridge)
docker compose ps
docker compose logs freeswitch | tail -20
```

#### Orchestrator가 ESL 연결 실패

```bash
# FreeSWITCH가 실행 중인지 확인
docker exec vbgw-freeswitch fs_cli -x "status"

# ESL 비밀번호 일치 확인
# .env의 ESL_PASSWORD와 FreeSWITCH의 event_socket.conf.xml이 같아야 합니다
docker exec vbgw-freeswitch cat /etc/freeswitch/autoload_configs/event_socket.conf.xml
```

#### One-Way Audio (한쪽만 들림)

```bash
# 1. NAT 설정 확인
echo $EXTERNAL_RTP_IP
# NAT 환경이면 공인 IP를 설정해야 합니다

# 2. RTP 포트 방화벽 확인
# UDP 16384-16584 포트가 열려 있어야 합니다

# 3. 코덱 협상 확인
docker exec vbgw-freeswitch fs_cli -x "sofia status profile internal"
```

#### 통화 중 끊김

```bash
# 1. 세션 타임아웃 확인
echo $SESSION_TIMEOUT
# 0이면 타임아웃 없음, 1800이면 30분 후 강제 종료

# 2. gRPC 스트림 데드라인 확인
echo $GRPC_STREAM_DEADLINE_SECS
# 기본 7200초 (2시간). 긴 통화는 이 값을 늘려야 합니다

# 3. FreeSWITCH RTP 타임아웃 확인 (30초 무음시 종료)
# config/freeswitch/sip_profiles/internal.xml의 rtp-timeout-sec 참조
```

#### DTMF 미작동

```bash
# 1. DTMF 모드 확인 (RFC 2833 권장)
docker exec vbgw-freeswitch fs_cli -x "sofia status profile internal" | grep dtmf

# 2. API로 DTMF 테스트
curl -X POST http://127.0.0.1:8080/api/v1/calls/CALL_ID/dtmf \
  -H "Authorization: Bearer YOUR_API_KEY" \
  -d '{"digits":"1"}'
```

---

## 10. 디렉토리 구조

```
vbgw-freeswitch/
|
+-- docker-compose.yml          # 개발용 3-tier 스택 정의
+-- docker-compose.prod.yml     # 프로덕션 오버레이 (TLS, 리소스 제한)
+-- .env.example                # 환경변수 템플릿 (40+ 변수)
|
+-- config/
|   +-- freeswitch/
|       +-- vars.xml                        # 환경변수 매핑
|       +-- sip_profiles/
|       |   +-- internal.xml                # Sofia SIP 프로파일
|       +-- dialplan/
|       |   +-- default.xml                 # 인바운드 다이얼플랜
|       +-- autoload_configs/
|           +-- event_socket.conf.xml       # ESL 설정
|           +-- acl.conf.xml                # SIP 소스 IP ACL
|           +-- conference.conf.xml         # 감독자/3-way 회의
|
+-- orchestrator/               # Tier 2: Go 오케스트레이터
|   +-- Dockerfile
|   +-- go.mod / go.sum
|   +-- cmd/main.go             # 진입점 (DI, graceful shutdown)
|   +-- internal/
|       +-- config/config.go    # 환경변수 로더
|       +-- esl/
|       |   +-- client.go       # ESL TCP 클라이언트 (auto-reconnect)
|       |   +-- commands.go     # 15개 ESL 명령 래퍼
|       |   +-- event.go        # 이벤트 파서
|       |   +-- interface.go    # Commander 인터페이스 (테스트용)
|       +-- session/
|       |   +-- manager.go      # 세션 관리 (sync.Map + atomic CAS)
|       |   +-- model.go        # SessionState 모델
|       +-- ivr/machine.go      # 5-state IVR FSM
|       +-- api/
|       |   +-- server.go       # chi 라우터, 14 엔드포인트
|       |   +-- middleware.go   # 인증 + rate limit + loopback
|       |   +-- calls.go        # POST /api/v1/calls
|       |   +-- control.go      # DTMF, transfer, record, bridge, eavesdrop
|       |   +-- stats.go        # GET /api/v1/calls/{id}/stats
|       |   +-- health.go       # /live, /ready, /health
|       +-- cdr/logger.go       # JSON CDR 로거
|       +-- recording/cleaner.go # 녹음 정리 (age + quota)
|       +-- metrics/prometheus.go # 20+ Prometheus 메트릭
|
+-- bridge/                     # Tier 3: Go WebSocket 브릿지
|   +-- Dockerfile
|   +-- go.mod / go.sum
|   +-- cmd/main.go             # 진입점
|   +-- internal/
|       +-- config/config.go    # 환경변수 로더
|       +-- ws/
|       |   +-- server.go       # WS 업그레이드 + Internal API
|       |   +-- session.go      # 4-goroutine 파이프라인
|       +-- vad/
|       |   +-- silero.go       # ONNX VAD (cgo 빌드 태그)
|       |   +-- silero_stub.go  # Energy-based VAD (!cgo 폴백)
|       |   +-- constants.go    # VAD 공통 상수
|       +-- grpc/
|       |   +-- client.go       # gRPC 양방향 스트리밍 + Pool
|       |   +-- retry.go        # 지수 백오프 재연결
|       +-- tts/buffer.go       # TTS 버퍼 (cap=200, oldest-drop)
|       +-- barge/controller.go  # Barge-in 2단계 제어
|
+-- protos/
|   +-- voicebot.proto          # gRPC 인터페이스 정의
|
+-- models/
|   +-- silero_vad.onnx         # Silero VAD v4 모델 (바이너리)
|
+-- scripts/
|   +-- run_tc_all.sh           # TC-01~TC-13 테스트 스위트
|   +-- run_load_test.sh        # SIPp 부하 테스트 + SLO
|   +-- slo_monitor.sh          # 실시간 SLO 모니터링
|   +-- memory_monitor.sh       # 72h 메모리 누수 감지
|   +-- validate_prod.sh        # 프로덕션 사전 검증
|   +-- cutover.sh              # Blue/Green 단계별 전환
|
+-- tests/
|   +-- sipp/
|       +-- load_test_uac.xml   # SIPp 부하 시나리오
|       +-- dtmf_scenario.xml   # DTMF IVR 테스트
|
+-- docs/
|   +-- operations_runbook.md   # 운영 런북
|   +-- troubleshooting.md      # 증상별 진단 매트릭스
|   +-- grafana_queries.md      # PromQL 쿼리 + 알람 조건
|
+-- recordings/                 # 녹음 파일 저장 (런타임)
+-- logs/                       # 로그 (런타임)
```

---

## 11. 보안 가이드

### 프로덕션 체크리스트

- [ ] `ESL_PASSWORD` — 강력한 랜덤 비밀번호 (`openssl rand -hex 16`)
- [ ] `ADMIN_API_KEY` — 최소 32자 (`openssl rand -hex 32`)
- [ ] `PBX_ACL_IP1/IP2` — PBX/SBC IP만 허용 (CIDR 형식, 예: `10.10.1.100/32`)
- [ ] `SIP_TLS_ONLY=true` — SIP TLS 강제
- [ ] `SRTP_MODE=mandatory` — SRTP 암호화 강제
- [ ] `AUDIO_FORK_SCHEME=wss` — 내부 WebSocket도 TLS
- [ ] `RUNTIME_PROFILE=production` — pprof 디버그 엔드포인트 비활성화
- [ ] 방화벽: SIP(5061), RTP(16384-16584 UDP), API(8080)만 허용
- [ ] ESL(8021), WS(8090), Internal(8091)은 외부 차단 (loopback only)

### 적용된 보안 기능

| 영역 | 기능 | 설명 |
|------|------|------|
| SIP | mod_acl | PBX IP만 INVITE 허용 |
| SIP | calls-per-second=30 | SIP 플러딩 방어 |
| SIP | accept-blind-transfer=false | Call hijacking 방지 |
| API | ConstantTimeCompare 인증 | 타이밍 공격 방어 |
| API | Per-IP + 전역 Rate Limit | DDoS 방어 |
| API | DTMF/Transfer 입력 정규식 검증 | ESL 인젝션 방지 |
| API | target_uri PII 마스킹 | 로그 개인정보 보호 |
| WebSocket | Loopback IP만 허용 | 외부 접근 차단 |
| ESL | 127.0.0.1만 수신 | 외부 ESL 접근 차단 |
| 내부 API | LoopbackOnlyMiddleware | barge-in 무인증 보호 |

---

## 12. 라이선스

이 프로젝트는 내부 사용 목적으로 개발되었습니다.

---

## 버전 정보

| 항목 | 값 |
|------|-----|
| 현재 버전 | v3.1.0 |
| FreeSWITCH | 1.10.12 |
| Go | 1.21+ |
| 테스트 수 | 133개 (22 파일) |
| API 엔드포인트 | 14개 |
| Prometheus 메트릭 | 20+ |
