# VBGW 로컬 테스트 가이드 (완전 초보자용)

> 이 문서는 VBGW FreeSWITCH v3.1.0을 로컬 PC에서 직접 설치하고 테스트하는 전 과정을 단계별로 설명합니다.
> 리눅스/Mac 터미널 사용이 처음인 분도 따라할 수 있도록 모든 명령어와 예상 결과를 포함합니다.

---

## 목차

- [Part 1: 환경 준비](#part-1-환경-준비)
- [Part 2: 프로젝트 설치](#part-2-프로젝트-설치)
- [Part 3: 서비스 시작](#part-3-서비스-시작)
- [Part 4: 정상 동작 확인](#part-4-정상-동작-확인)
- [Part 5: API 테스트](#part-5-api-테스트)
- [Part 6: 유닛 테스트 실행](#part-6-유닛-테스트-실행)
- [Part 7: SIP 통화 테스트 (SIPp)](#part-7-sip-통화-테스트-sipp)
- [Part 8: 부하 테스트](#part-8-부하-테스트)
- [Part 9: 로그 확인 및 디버깅](#part-9-로그-확인-및-디버깅)
- [Part 10: 정리 및 종료](#part-10-정리-및-종료)
- [Part 11: 자주 묻는 질문 (FAQ)](#part-11-자주-묻는-질문-faq)
- [부록 A: Mock AI 서버 만들기](#부록-a-mock-ai-서버-만들기)
- [부록 B: 용어 사전](#부록-b-용어-사전)

---

## Part 1: 환경 준비

### 1.1 운영체제 확인

이 가이드는 아래 환경에서 테스트되었습니다:

| OS | 지원 |
|-----|------|
| macOS 13+ (Ventura, Sonoma, Sequoia) | O |
| Ubuntu 22.04 / 24.04 | O |
| Windows 11 + WSL2 | O (WSL 내에서 실행) |

Windows 사용자는 반드시 WSL2를 먼저 설치해야 합니다.

### 1.2 Docker Desktop 설치

Docker는 VBGW의 3개 서비스(FreeSWITCH, Orchestrator, Bridge)를 한 번에 실행하는 핵심 도구입니다.

#### macOS

```bash
# 1. Homebrew가 없다면 먼저 설치
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# 2. Docker Desktop 설치
brew install --cask docker

# 3. Docker Desktop 앱 실행 (Launchpad 또는 Spotlight에서 "Docker" 검색)
open -a Docker

# 4. Docker가 실행될 때까지 30초~1분 대기 후 확인
docker --version
# 기대 결과: Docker version 27.x.x, build xxxxxxx

docker compose version
# 기대 결과: Docker Compose version v2.xx.x
```

> **중요**: Docker Desktop 앱이 상단 메뉴바에 고래 아이콘으로 표시되어야 합니다.
> 아이콘이 없으면 Docker가 실행되지 않은 것입니다.

#### Ubuntu / WSL2

```bash
# 1. 이전 버전 제거 (처음 설치라면 건너뛰기)
sudo apt-get remove docker docker-engine docker.io containerd runc 2>/dev/null

# 2. 필수 패키지 설치
sudo apt-get update
sudo apt-get install -y ca-certificates curl gnupg

# 3. Docker 공식 GPG 키 추가
sudo install -m 0755 -d /etc/apt/keyrings
curl -fsSL https://download.docker.com/linux/ubuntu/gpg | sudo gpg --dearmor -o /etc/apt/keyrings/docker.gpg

# 4. Docker 저장소 추가
echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] https://download.docker.com/linux/ubuntu $(. /etc/os-release && echo "$VERSION_CODENAME") stable" | sudo tee /etc/apt/sources.list.d/docker.list > /dev/null

# 5. Docker 설치
sudo apt-get update
sudo apt-get install -y docker-ce docker-ce-cli containerd.io docker-compose-plugin

# 6. 현재 사용자를 docker 그룹에 추가 (sudo 없이 docker 사용)
sudo usermod -aG docker $USER

# 7. 그룹 변경 적용 (로그아웃 후 재로그인 또는 아래 명령 실행)
newgrp docker

# 8. 확인
docker --version
docker compose version
```

### 1.3 Git 설치

```bash
# macOS (Homebrew)
brew install git

# Ubuntu
sudo apt-get install -y git

# 확인
git --version
# 기대 결과: git version 2.3x.x 이상
```

### 1.4 추가 도구 설치 (선택)

```bash
# jq: JSON 응답을 보기 좋게 출력하는 도구
# macOS
brew install jq

# Ubuntu
sudo apt-get install -y jq

# 확인
jq --version
# 기대 결과: jq-1.7.x
```

### 1.5 Go 설치 (유닛 테스트 실행 시 필요)

유닛 테스트를 로컬에서 직접 실행하려면 Go가 필요합니다.
Docker만 사용한다면 이 단계는 건너뛰어도 됩니다.

```bash
# macOS
brew install go

# Ubuntu
sudo apt-get install -y golang-go
# 또는 최신 버전 직접 설치:
# wget https://go.dev/dl/go1.23.0.linux-amd64.tar.gz
# sudo tar -C /usr/local -xzf go1.23.0.linux-amd64.tar.gz
# echo 'export PATH=$PATH:/usr/local/go/bin' >> ~/.bashrc && source ~/.bashrc

# 확인
go version
# 기대 결과: go version go1.22.x 이상
```

### 1.6 SIPp 설치 (SIP 통화 테스트 시 필요)

SIPp는 SIP 프로토콜로 가짜 전화를 걸어서 VBGW를 테스트하는 도구입니다.
API 테스트만 할 거라면 건너뛰어도 됩니다.

```bash
# macOS
brew install sipp

# Ubuntu
sudo apt-get install -y sip-tester

# 확인
sipp -v
# 기대 결과: SIPp v3.x.x 이상
```

---

## Part 2: 프로젝트 설치

### 2.1 소스코드 다운로드

```bash
# 1. 원하는 작업 디렉토리로 이동
cd ~

# 2. 저장소 클론
git clone https://github.com/nayupuel/vbgw_v2.git

# 3. FreeSWITCH 프로젝트 디렉토리로 이동
cd vbgw_v2/vbgw-freeswitch

# 4. 현재 위치 확인
pwd
# 기대 결과: /Users/사용자이름/vbgw_v2/vbgw-freeswitch  (macOS)
# 또는:      /home/사용자이름/vbgw_v2/vbgw-freeswitch    (Ubuntu)
```

### 2.2 환경변수 파일 생성

```bash
# 1. 템플릿 복사
cp .env.example .env

# 2. 비밀번호 자동 생성 및 설정
#    아래 명령을 하나씩 실행하면 .env 파일의 해당 값이 자동으로 바뀝니다.

# ESL 비밀번호 생성 (FreeSWITCH ↔ Orchestrator 통신용)
ESL_PW=$(openssl rand -hex 16)
echo "생성된 ESL 비밀번호: ${ESL_PW}"

# Admin API Key 생성 (REST API 인증용)
API_KEY=$(openssl rand -hex 32)
echo "생성된 API Key: ${API_KEY}"

# .env 파일에 반영 (macOS와 Linux 모두 호환)
if [[ "$OSTYPE" == "darwin"* ]]; then
    # macOS
    sed -i '' "s|ESL_PASSWORD=.*|ESL_PASSWORD=${ESL_PW}|" .env
    sed -i '' "s|ADMIN_API_KEY=.*|ADMIN_API_KEY=${API_KEY}|" .env
else
    # Linux
    sed -i "s|ESL_PASSWORD=.*|ESL_PASSWORD=${ESL_PW}|" .env
    sed -i "s|ADMIN_API_KEY=.*|ADMIN_API_KEY=${API_KEY}|" .env
fi

# 3. 설정 확인
grep "ESL_PASSWORD=" .env
grep "ADMIN_API_KEY=" .env
# 두 줄 모두 긴 hex 문자열이 보이면 성공
```

> **절대 주의**: `.env` 파일에는 비밀번호가 들어있습니다.
> 이 파일을 Git에 커밋하거나 다른 사람에게 공유하지 마세요.
> `.gitignore`에 이미 등록되어 있어서 실수로 커밋되지는 않습니다.

### 2.3 필수 디렉토리 생성

```bash
mkdir -p recordings logs/freeswitch logs/orchestrator logs/bridge models
```

각 디렉토리의 용도:

| 디렉토리 | 용도 |
|----------|------|
| `recordings/` | 통화 녹음 파일 저장 |
| `logs/freeswitch/` | FreeSWITCH 로그 |
| `logs/orchestrator/` | Orchestrator 로그 |
| `logs/bridge/` | Bridge 로그 |
| `models/` | VAD 모델 파일 (silero_vad.onnx) |

### 2.4 VAD 모델 확인

```bash
ls -la models/silero_vad.onnx
# 파일이 있으면 OK (약 1.8MB)
# 파일이 없어도 Bridge는 energy-based 폴백으로 동작합니다 (정확도만 낮음)
```

### 2.5 .env 전체 확인 (최종 점검)

```bash
cat .env
```

확인할 핵심 항목:

```
ESL_PASSWORD=a1b2c3d4...     ← 긴 hex 문자열이어야 함 (빈 값 X)
ADMIN_API_KEY=e5f6g7h8...    ← 긴 hex 문자열이어야 함 (빈 값 X)
AI_GRPC_ADDR=127.0.0.1:50051 ← AI 서버 주소 (로컬 테스트에서는 이 값 유지)
RUNTIME_PROFILE=dev           ← dev여야 함 (production이면 디버그 기능 비활성화)
```

---

## Part 3: 서비스 시작

### 3.1 Docker 이미지 빌드 및 시작

```bash
# 1. Docker Desktop이 실행 중인지 확인
docker info > /dev/null 2>&1 && echo "Docker is running" || echo "ERROR: Docker is not running!"
# "Docker is running"이 출력되어야 합니다

# 2. 서비스 빌드 + 시작 (처음에는 2~5분 소요)
docker compose up -d --build
```

`--build` 옵션은 Orchestrator와 Bridge의 Go 코드를 컴파일합니다.
처음 실행할 때만 오래 걸리고, 이후에는 캐시되어 빠릅니다.

예상 출력:
```
[+] Building 120.5s (18/18) FINISHED
 => [orchestrator] ...
 => [bridge] ...
[+] Running 3/3
 ✔ Container vbgw-freeswitch    Started
 ✔ Container vbgw-orchestrator  Started
 ✔ Container vbgw-bridge        Started
```

### 3.2 서비스 시작 확인

```bash
# 1. 전체 상태 확인
docker compose ps
```

**정상 상태** (1~2분 후):
```
NAME                 STATUS          PORTS
vbgw-freeswitch      Up (healthy)
vbgw-orchestrator    Up (healthy)
vbgw-bridge          Up (healthy)
```

**아직 시작 중** (30초 이내):
```
NAME                 STATUS
vbgw-freeswitch      Up (health: starting)
vbgw-orchestrator    Up (health: starting)
vbgw-bridge          Up (health: starting)
```

> `health: starting`이면 30초 더 기다린 후 다시 확인하세요.

**에러가 있는 경우**:
```
NAME                 STATUS
vbgw-freeswitch      Up (unhealthy)
vbgw-orchestrator    Restarting
```

에러 시 [Part 9: 로그 확인 및 디버깅](#part-9-로그-확인-및-디버깅)으로 이동하세요.

### 3.3 서비스 시작 순서 이해

```
1. FreeSWITCH 시작     (SIP 엔진 + 미디어 처리)
       ↓ healthcheck 통과 후
2. Orchestrator 시작   (ESL로 FreeSWITCH에 연결)
       ↓
3. Bridge 시작         (WebSocket + gRPC 준비)
```

FreeSWITCH가 healthy 상태가 되어야 나머지 두 서비스가 시작됩니다.
FreeSWITCH 시작에 약 15-30초가 소요됩니다.

---

## Part 4: 정상 동작 확인

3개 서비스가 모두 healthy가 된 후 아래 테스트를 진행합니다.

### 4.1 Liveness 체크 (서비스가 살아있는지)

```bash
curl -s http://127.0.0.1:8080/live
```

기대 결과:
```json
{"status":"ok"}
```

> 이 엔드포인트는 인증이 필요 없습니다 (K8s liveness probe용).

### 4.2 Readiness 체크 (모든 연결이 정상인지)

```bash
curl -s http://127.0.0.1:8080/ready
```

기대 결과:
```json
{"status":"ready"}
```

> `ready`가 나오면 Orchestrator → FreeSWITCH(ESL) 연결, Orchestrator → Bridge 연결 모두 정상입니다.

### 4.3 상세 헬스 체크 (인증 필요)

```bash
# .env에서 설정한 API Key를 사용합니다
# 아래 명령에서 API Key를 자동으로 읽어옵니다
API_KEY=$(grep "ADMIN_API_KEY=" .env | cut -d= -f2)

curl -s -H "Authorization: Bearer ${API_KEY}" http://127.0.0.1:8080/health | jq .
```

기대 결과:
```json
{
  "status": "healthy",
  "esl_connected": true,
  "bridge_healthy": true,
  "active_calls": 0,
  "max_sessions": 100
}
```

각 필드 설명:

| 필드 | 의미 | 정상 값 |
|------|------|---------|
| `status` | 전체 상태 | `"healthy"` |
| `esl_connected` | FreeSWITCH ESL 연결 | `true` |
| `bridge_healthy` | Bridge 서비스 응답 | `true` |
| `active_calls` | 현재 진행 중인 통화 수 | `0` (테스트 전) |
| `max_sessions` | 최대 동시 통화 가능 수 | `100` (기본값) |

### 4.4 Bridge 내부 상태 체크

```bash
curl -s http://127.0.0.1:8091/internal/health
```

기대 결과:
```json
{"status":"ok","active_sessions":0}
```

### 4.5 FreeSWITCH SIP 상태 확인

```bash
docker exec vbgw-freeswitch fs_cli -x "sofia status"
```

기대 결과 (일부):
```
                     Name          Type                                       Data      State
=================================================================================================
                 internal       profile            sip:mod_sofia@...:5060      RUNNING (0)
```

`RUNNING`이 보이면 SIP 프로파일이 정상 동작 중입니다.

### 4.6 Prometheus 메트릭 확인

```bash
API_KEY=$(grep "ADMIN_API_KEY=" .env | cut -d= -f2)

curl -s -H "Authorization: Bearer ${API_KEY}" http://127.0.0.1:8080/metrics | head -20
```

기대 결과 (일부):
```
# HELP vbgw_active_calls Current number of active calls
# TYPE vbgw_active_calls gauge
vbgw_active_calls 0
# HELP vbgw_total_calls Total number of calls processed
# TYPE vbgw_total_calls counter
vbgw_total_calls 0
...
```

### 4.7 체크리스트 요약

| # | 테스트 | 명령어 | 기대 결과 | 결과 |
|---|--------|--------|----------|------|
| 1 | Liveness | `curl http://127.0.0.1:8080/live` | `{"status":"ok"}` | |
| 2 | Readiness | `curl http://127.0.0.1:8080/ready` | `{"status":"ready"}` | |
| 3 | Health | `curl -H "Auth..." /health` | `esl_connected: true` | |
| 4 | Bridge | `curl http://127.0.0.1:8091/internal/health` | `{"status":"ok"}` | |
| 5 | Sofia | `docker exec ... fs_cli -x "sofia status"` | `RUNNING` | |
| 6 | Metrics | `curl -H "Auth..." /metrics` | `vbgw_active_calls 0` | |

6개 모두 통과하면 서비스가 정상 가동 중입니다.

---

## Part 5: API 테스트

### 5.0 준비: API Key 변수 설정

매번 긴 API Key를 입력하지 않도록 환경변수로 설정합니다:

```bash
export API_KEY=$(grep "ADMIN_API_KEY=" .env | cut -d= -f2)

# 확인 (값이 출력되어야 함)
echo $API_KEY
```

### 5.1 아웃바운드 콜 생성

실제 PBX 없이도 API 동작을 확인할 수 있습니다.
(SIP 연결은 실패하지만 API 자체의 응답을 확인합니다)

```bash
curl -s -X POST http://127.0.0.1:8080/api/v1/calls \
  -H "Authorization: Bearer ${API_KEY}" \
  -H "Content-Type: application/json" \
  -d '{"target_uri": "1001@127.0.0.1"}' | jq .
```

기대 결과 (201 Created):
```json
{
  "call_id": "abc123-def456-...",
  "fs_uuid": "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx",
  "status": "initiating"
}
```

> `call_id`는 VBGW가 관리하는 세션 ID입니다.
> `fs_uuid`는 FreeSWITCH 내부 채널 UUID입니다.
> 실제 PBX가 없으므로 콜은 성립되지 않지만, API 처리는 정상 동작합니다.

### 5.2 잘못된 요청 테스트 (400 에러)

```bash
# target_uri가 비어있는 경우
curl -s -X POST http://127.0.0.1:8080/api/v1/calls \
  -H "Authorization: Bearer ${API_KEY}" \
  -H "Content-Type: application/json" \
  -d '{"target_uri": ""}' | jq .
```

기대 결과 (400 Bad Request):
```json
{"error": "target_uri required"}
```

### 5.3 인증 실패 테스트 (401 에러)

```bash
# 잘못된 API Key
curl -s -X POST http://127.0.0.1:8080/api/v1/calls \
  -H "Authorization: Bearer wrong-key" \
  -H "Content-Type: application/json" \
  -d '{"target_uri": "1001@127.0.0.1"}'
```

기대 결과 (401 Unauthorized):
```
Unauthorized
```

### 5.4 DTMF 전송 테스트 (세션 없는 경우)

```bash
curl -s -X POST http://127.0.0.1:8080/api/v1/calls/nonexistent-id/dtmf \
  -H "Authorization: Bearer ${API_KEY}" \
  -H "Content-Type: application/json" \
  -d '{"digits": "1234"}' | jq .
```

기대 결과 (404 Not Found):
```json
{"error": "session not found"}
```

### 5.5 DTMF 유효성 검증 테스트

```bash
# 허용되지 않는 문자 포함
curl -s -X POST http://127.0.0.1:8080/api/v1/calls/some-id/dtmf \
  -H "Authorization: Bearer ${API_KEY}" \
  -H "Content-Type: application/json" \
  -d '{"digits": "abc!@#"}' | jq .
```

기대 결과 (400 또는 404):
```json
{"error": "session not found"}
```

> DTMF digits는 `0-9`, `*`, `#`, `A-D`만 허용됩니다 (최대 20자).

### 5.6 Rate Limit 테스트

API 요청을 빠르게 반복하면 rate limit에 걸립니다:

```bash
# 50번 빠르게 요청
for i in $(seq 1 50); do
  CODE=$(curl -s -o /dev/null -w "%{http_code}" \
    -H "Authorization: Bearer ${API_KEY}" \
    http://127.0.0.1:8080/health)
  echo "Request $i: HTTP $CODE"
done
```

기대 결과: 처음 40개 정도는 `200`, 이후 `429 Too Many Requests`가 나타남.

### 5.7 API 테스트 체크리스트

| # | 테스트 | HTTP 코드 | 결과 |
|---|--------|----------|------|
| 1 | POST /api/v1/calls (정상) | 201 | |
| 2 | POST /api/v1/calls (빈 target) | 400 | |
| 3 | POST /api/v1/calls (잘못된 키) | 401 | |
| 4 | POST /calls/{id}/dtmf (없는 세션) | 404 | |
| 5 | Rate limit 초과 | 429 | |

---

## Part 6: 유닛 테스트 실행

### 6.1 Orchestrator 테스트 (89개)

```bash
cd orchestrator

# 테스트 실행
CGO_ENABLED=0 go test -count=1 -v ./...
```

`-v` 옵션은 각 테스트의 이름과 결과를 모두 보여줍니다.

기대 결과 (마지막 부분):
```
ok      vbgw-orchestrator/internal/api          0.015s
ok      vbgw-orchestrator/internal/esl          0.003s
ok      vbgw-orchestrator/internal/session       0.002s
ok      vbgw-orchestrator/internal/ivr           0.004s
ok      vbgw-orchestrator/internal/metrics       0.001s
ok      vbgw-orchestrator/internal/cdr           0.001s
ok      vbgw-orchestrator/internal/recording     0.001s
PASS
```

> `FAIL`이 하나도 없으면 성공입니다.

### 6.2 Bridge 테스트 (44개)

```bash
cd ../bridge

# CGO 없이 실행 (energy-based VAD stub 사용)
CGO_ENABLED=0 go test -count=1 -v ./...
```

기대 결과:
```
ok      vbgw-bridge/internal/ws         0.012s
ok      vbgw-bridge/internal/grpc       0.005s
ok      vbgw-bridge/internal/vad        0.002s
ok      vbgw-bridge/internal/tts        0.001s
ok      vbgw-bridge/internal/barge      0.001s
PASS
```

### 6.3 커버리지 측정

```bash
# Orchestrator 커버리지
cd ../orchestrator
go test -coverprofile=coverage.out ./...
go tool cover -func=coverage.out | tail -1
# 기대 결과: total:    (statements)    XX.X%

# Bridge 커버리지
cd ../bridge
CGO_ENABLED=0 go test -coverprofile=coverage.out ./...
go tool cover -func=coverage.out | tail -1
```

### 6.4 특정 패키지만 테스트

특정 패키지의 테스트만 실행하고 싶을 때:

```bash
# API 핸들러만 테스트
cd ../orchestrator
CGO_ENABLED=0 go test -v ./internal/api/...

# ESL 클라이언트만 테스트
CGO_ENABLED=0 go test -v ./internal/esl/...

# Bridge WebSocket만 테스트
cd ../bridge
CGO_ENABLED=0 go test -v ./internal/ws/...
```

### 6.5 특정 테스트 함수만 실행

```bash
# TestCreateCall_Success만 실행
cd ../orchestrator
CGO_ENABLED=0 go test -v -run TestCreateCall_Success ./internal/api/...

# Test 이름에 "DTMF"가 포함된 테스트만
CGO_ENABLED=0 go test -v -run "DTMF" ./internal/api/...
```

### 6.6 테스트 목록 (133개)

| 패키지 | 테스트 수 | 주요 테스트 |
|--------|----------|------------|
| api/middleware | 8 | Auth, RateLimit, Loopback, Metrics |
| api/calls | 7 | CreateCall 성공/실패/용량/JSON/ESL에러, maskURI |
| api/control | 9 | DTMF, Transfer, Record, Bridge, BargeIn |
| api/stats | 3 | GetStats 성공/404/ESL에러 |
| api/health | 여러 | Liveness, Readiness, Health 조합 |
| esl/client | 8 | 연결, 콜백, apiRespCh, Commander 인터페이스 |
| grpc/client | 10 | Pool, Stream Send/Recv, DTMF, 컨텍스트 |
| ws/server | 7 | Internal API (health, pause, resume, dtmf, shutdown) |
| ws/session | 4 | AI pause, 채널 오버플로우, 용량 |
| 기타 | 여러 | session, ivr, cdr, recording, vad, tts, barge |

---

## Part 7: SIP 통화 테스트 (SIPp)

> **전제조건**: SIPp가 설치되어 있어야 합니다 ([1.6 SIPp 설치](#16-sipp-설치-sip-통화-테스트-시-필요) 참조).

### 7.1 SIPp란?

SIPp는 가짜 SIP 전화를 생성하는 테스트 도구입니다.
실제 전화기 없이 VBGW에 SIP INVITE를 보내고 통화를 시뮬레이션합니다.

### 7.2 단일 통화 테스트

```bash
# vbgw-freeswitch/ 디렉토리에서 실행
cd ~/vbgw_v2/vbgw-freeswitch

# 1통화, 3초 유지 후 종료
sipp -sn uac \
  -d 3000 \
  -l 1 \
  -m 1 \
  127.0.0.1:5060 \
  -timeout 10
```

각 옵션 설명:

| 옵션 | 의미 |
|------|------|
| `-sn uac` | 내장 UAC(발신자) 시나리오 사용 |
| `-d 3000` | 통화 유지 시간 3000ms (3초) |
| `-l 1` | 동시 통화 수 1 |
| `-m 1` | 총 통화 수 1 |
| `127.0.0.1:5060` | FreeSWITCH SIP 주소 |
| `-timeout 10` | 10초 타임아웃 |

기대 결과:
```
------------------------------ Scenario Screen ----...
  Call rate (length)   Port   Total-time  ...
  1.0(3000 ms)/1.000s  ...    ...

  0 new calls during ... period  ...  ...  1 ...
```

### 7.3 통화 중 API로 상태 확인

다른 터미널 탭을 열고 통화 중에 확인합니다:

```bash
API_KEY=$(grep "ADMIN_API_KEY=" ~/vbgw_v2/vbgw-freeswitch/.env | cut -d= -f2)

# 활성 통화 확인
curl -s -H "Authorization: Bearer ${API_KEY}" \
  http://127.0.0.1:8080/health | jq .active_calls
# 기대: 1 (통화 중일 때)

# 통화 종료 후
# 기대: 0
```

### 7.4 커스텀 시나리오 테스트

VBGW 전용 부하 테스트 시나리오를 사용합니다:

```bash
# SDP 포함 시나리오 (G.711 코덱 + DTMF)
sipp -sf tests/sipp/load_test_uac.xml \
  -d 5000 \
  -l 1 \
  -m 1 \
  127.0.0.1:5060 \
  -timeout 15
```

### 7.5 동시 10통화 테스트

```bash
sipp -sn uac \
  -d 5000 \
  -l 10 \
  -m 10 \
  -r 2 \
  127.0.0.1:5060 \
  -timeout 30
```

| 옵션 | 의미 |
|------|------|
| `-l 10` | 동시 10통화 |
| `-m 10` | 총 10통화 |
| `-r 2` | 초당 2통화씩 생성 |

---

## Part 8: 부하 테스트

### 8.1 자동 부하 테스트 스크립트

```bash
cd ~/vbgw_v2/vbgw-freeswitch

# 100 동시콜, 5분 유지, SLO 모니터링 포함
bash scripts/run_load_test.sh
```

이 스크립트는 다음을 자동으로 수행합니다:
1. SIPp로 100 동시콜 생성
2. SLO 모니터링 (health, active_calls, errors 추적)
3. 메모리 사용량 수집
4. 결과 리포트 생성

### 8.2 전체 테스트 케이스 실행 (TC-01 ~ TC-13)

```bash
# 환경변수 설정
export ADMIN_API_KEY=$(grep "ADMIN_API_KEY=" .env | cut -d= -f2)

# 전체 실행
bash scripts/run_tc_all.sh 127.0.0.1
```

결과:
```
============================================
 TC-01 ~ TC-08 Automated Test Suite
============================================
--- TC-01: SIP Call Setup / Teardown ---
  ✓ PASS: Single call setup/teardown
  ✓ PASS: active_calls = 0 after hangup
...
============================================
 Results: 13 PASS / 0 FAIL
============================================
```

### 8.3 메모리 누수 감지 (장시간)

```bash
# 72시간 모니터링 (pprof heap 주기적 수집)
bash scripts/memory_monitor.sh
```

> 이 스크립트는 장시간 실행됩니다. 별도 터미널이나 tmux/screen에서 실행하세요.

---

## Part 9: 로그 확인 및 디버깅

### 9.1 실시간 로그 보기

```bash
# 전체 서비스 로그 (Ctrl+C로 종료)
docker compose logs -f

# 특정 서비스만
docker compose logs -f freeswitch
docker compose logs -f orchestrator
docker compose logs -f bridge
```

### 9.2 최근 에러만 보기

```bash
# Orchestrator 에러 로그
docker compose logs orchestrator 2>&1 | grep -i "error\|fatal\|panic"

# Bridge 에러 로그
docker compose logs bridge 2>&1 | grep -i "error\|fatal\|panic"
```

### 9.3 FreeSWITCH CLI 접속

```bash
# FreeSWITCH 콘솔에 직접 접속 (고급)
docker exec -it vbgw-freeswitch fs_cli

# 나가기: /exit 또는 Ctrl+D
```

자주 사용하는 FreeSWITCH CLI 명령:

```bash
# Sofia SIP 상태
docker exec vbgw-freeswitch fs_cli -x "sofia status"

# 활성 통화 목록
docker exec vbgw-freeswitch fs_cli -x "show calls"

# 활성 채널 목록
docker exec vbgw-freeswitch fs_cli -x "show channels"

# ESL 연결 상태
docker exec vbgw-freeswitch fs_cli -x "event_socket status"

# 로드된 모듈 확인
docker exec vbgw-freeswitch fs_cli -x "module_exists mod_audio_fork"
```

### 9.4 자주 발생하는 에러와 해결법

#### 에러: "ESL connection refused"

```
Orchestrator 로그: "ESL connect failed: dial tcp 127.0.0.1:8021: connection refused"
```

**원인**: FreeSWITCH가 아직 시작되지 않았거나, ESL이 비활성 상태.

**해결**:
```bash
# 1. FreeSWITCH 상태 확인
docker compose ps freeswitch

# 2. FreeSWITCH 로그에서 에러 확인
docker compose logs freeswitch | tail -30

# 3. 재시작
docker compose restart freeswitch
# 30초 대기 후
docker compose restart orchestrator
```

#### 에러: "Unauthorized"

```
curl 응답: "Unauthorized"
```

**원인**: API Key가 틀리거나 누락.

**해결**:
```bash
# 1. .env의 API Key 확인
grep ADMIN_API_KEY .env

# 2. 정확한 Key로 요청
API_KEY=$(grep "ADMIN_API_KEY=" .env | cut -d= -f2)
curl -H "Authorization: Bearer ${API_KEY}" http://127.0.0.1:8080/health
```

#### 에러: "bridge_healthy: false"

```json
{"status":"degraded","esl_connected":true,"bridge_healthy":false}
```

**원인**: Bridge 서비스가 비정상.

**해결**:
```bash
# 1. Bridge 상태 확인
docker compose ps bridge
docker compose logs bridge | tail -20

# 2. Bridge 직접 헬스체크
curl -s http://127.0.0.1:8091/internal/health

# 3. 재시작
docker compose restart bridge
```

#### 에러: Docker 빌드 실패 "go mod download"

```
ERROR: failed to solve: go mod download: exit code 1
```

**원인**: 인터넷 연결 문제 또는 Go 모듈 프록시 차단.

**해결**:
```bash
# 1. 인터넷 연결 확인
curl -s https://proxy.golang.org

# 2. Go 프록시 설정 후 재빌드
export GOPROXY=https://proxy.golang.org,direct
docker compose build --no-cache
```

#### 에러: "port already in use"

```
Error: listen tcp 0.0.0.0:5060: bind: address already in use
```

**원인**: 5060 포트를 다른 프로세스가 사용 중.

**해결**:
```bash
# 어떤 프로세스가 포트를 사용 중인지 확인
# macOS
lsof -i :5060

# Linux
sudo ss -tlnp | grep 5060

# 해당 프로세스를 종료하거나, .env에서 SIP_PORT를 다른 값으로 변경
```

---

## Part 10: 정리 및 종료

### 10.1 서비스 중지

```bash
cd ~/vbgw_v2/vbgw-freeswitch

# 서비스 중지 (컨테이너 유지)
docker compose stop

# 서비스 중지 + 컨테이너 삭제
docker compose down
```

### 10.2 데이터까지 완전 삭제

```bash
# 서비스 + 볼륨 + 이미지 완전 삭제
docker compose down --rmi all --volumes

# 로그 + 녹음 파일 삭제
rm -rf logs/ recordings/
```

### 10.3 Docker 디스크 정리

```bash
# 사용하지 않는 Docker 리소스 정리
docker system prune -f

# 빌드 캐시까지 정리 (다음 빌드 시 느려짐)
docker builder prune -f
```

### 10.4 다시 시작하기

```bash
cd ~/vbgw_v2/vbgw-freeswitch

# 코드 최신화
git pull

# 재빌드 + 시작
docker compose up -d --build
```

---

## Part 11: 자주 묻는 질문 (FAQ)

### Q: AI 서버 없이도 테스트할 수 있나요?

**A**: 네. API 테스트, 유닛 테스트, SIP 시그널링 테스트는 AI 서버 없이 가능합니다.
실제 음성 대화 테스트만 AI 서버가 필요합니다.
AI 서버가 없으면 Bridge가 gRPC 연결 실패 로그를 출력하지만, 나머지 기능은 정상입니다.
Mock AI 서버가 필요하면 [부록 A](#부록-a-mock-ai-서버-만들기)를 참조하세요.

### Q: macOS에서 `network_mode: host`가 안 됩니다

**A**: Docker Desktop for Mac은 `network_mode: host`를 완전히 지원하지 않습니다.
하지만 VBGW의 각 서비스는 loopback(127.0.0.1)으로 통신하므로 대부분 정상 동작합니다.
외부 PBX 연결이 필요하면 Linux 환경을 사용하세요.

### Q: FreeSWITCH가 "unhealthy"로 표시됩니다

**A**: 아래 순서로 확인하세요:
```bash
# 1. 로그 확인
docker compose logs freeswitch | tail -30

# 2. Sofia 상태 직접 확인
docker exec vbgw-freeswitch fs_cli -x "sofia status"

# 3. 설정 파일 유효성 (XML 파싱 에러 등)
docker exec vbgw-freeswitch fs_cli -x "xml_locate configuration"
```

### Q: 테스트 중 "too many open files" 에러가 납니다

**A**: 파일 디스크립터 제한을 늘려야 합니다:
```bash
# 현재 제한 확인
ulimit -n

# 제한 늘리기 (현재 세션)
ulimit -n 65535
```

### Q: Go 테스트에서 "module not found" 에러

**A**: `go.sum` 파일이 없거나 의존성을 받지 않은 경우:
```bash
cd orchestrator && go mod tidy && go mod download
cd ../bridge && go mod tidy && go mod download
```

### Q: 실제 PBX 연동은 어떻게 하나요?

**A**: `.env`에 PBX 정보를 설정합니다:
```bash
PBX_HOST=10.10.1.100        # PBX IP
PBX_USERNAME=vbgw            # SIP 사용자
PBX_PASSWORD=your-password   # SIP 비밀번호
PBX_REGISTER=true            # PBX에 등록

# 보안: PBX IP만 허용
PBX_ACL_IP1=10.10.1.100/32
```

---

## 부록 A: Mock AI 서버 만들기

VBGW의 전체 음성 파이프라인을 테스트하려면 gRPC AI 서버가 필요합니다.
아래는 Python으로 간단한 Mock AI 서버를 만드는 방법입니다.

### A.1 사전 준비

```bash
# Python 3.9+ 필요
python3 --version

# 가상환경 생성
cd ~/vbgw_v2/vbgw-freeswitch
python3 -m venv mock_ai_venv
source mock_ai_venv/bin/activate

# 의존성 설치
pip install grpcio grpcio-tools
```

### A.2 Proto 컴파일

```bash
python3 -m grpc_tools.protoc \
  -I protos/ \
  --python_out=. \
  --grpc_python_out=. \
  protos/voicebot.proto
```

이 명령은 2개 파일을 생성합니다:
- `voicebot_pb2.py` — 메시지 정의
- `voicebot_pb2_grpc.py` — gRPC 서비스 정의

### A.3 Mock 서버 작성

`mock_ai_server.py` 파일을 생성합니다:

```python
"""
Mock AI Server — VBGW 로컬 테스트용
오디오를 받으면 "안녕하세요" TTS 응답을 반환합니다.
"""
import time
import struct
import grpc
from concurrent import futures

import voicebot_pb2
import voicebot_pb2_grpc


class MockAiService(voicebot_pb2_grpc.VoicebotAiServiceServicer):
    def StreamSession(self, request_iterator, context):
        print("[Mock AI] New session started")
        chunk_count = 0

        for chunk in request_iterator:
            chunk_count += 1

            # 매 50번째 청크(약 1초)마다 STT 결과 반환
            if chunk_count % 50 == 0:
                print(f"[Mock AI] Session={chunk.session_id}, chunks={chunk_count}, speaking={chunk.is_speaking}")

                # STT 결과 전송
                yield voicebot_pb2.AiResponse(
                    type=voicebot_pb2.AiResponse.STT_RESULT,
                    text_content=f"[STT] 테스트 발화 (chunk #{chunk_count})",
                )

            # DTMF 이벤트 처리
            if chunk.dtmf_digit:
                print(f"[Mock AI] DTMF received: {chunk.dtmf_digit}")
                yield voicebot_pb2.AiResponse(
                    type=voicebot_pb2.AiResponse.STT_RESULT,
                    text_content=f"[DTMF] {chunk.dtmf_digit} pressed",
                )

            # 매 100번째 청크마다 TTS 응답 (무음 PCM)
            if chunk_count % 100 == 0:
                silence = struct.pack("<" + "h" * 320, *([0] * 320))  # 20ms silence
                yield voicebot_pb2.AiResponse(
                    type=voicebot_pb2.AiResponse.TTS_AUDIO,
                    audio_data=silence,
                )
                yield voicebot_pb2.AiResponse(
                    type=voicebot_pb2.AiResponse.END_OF_TURN,
                )

        print(f"[Mock AI] Session ended, total chunks: {chunk_count}")


def serve():
    server = grpc.server(futures.ThreadPoolExecutor(max_workers=10))
    voicebot_pb2_grpc.add_VoicebotAiServiceServicer_to_server(MockAiService(), server)
    server.add_insecure_port("[::]:50051")
    server.start()
    print("[Mock AI] Server started on port 50051")
    print("[Mock AI] Waiting for connections...")
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        print("\n[Mock AI] Shutting down...")
        server.stop(5)


if __name__ == "__main__":
    serve()
```

### A.4 Mock 서버 실행

```bash
python3 mock_ai_server.py
```

기대 출력:
```
[Mock AI] Server started on port 50051
[Mock AI] Waiting for connections...
```

이제 VBGW Bridge가 이 서버에 연결하여 음성 파이프라인 전체를 테스트할 수 있습니다.

### A.5 Mock 서버 종료

`Ctrl+C`로 종료합니다.

---

## 부록 B: 용어 사전

| 용어 | 설명 |
|------|------|
| **SIP** | Session Initiation Protocol. 전화를 걸고 받는 시그널링 프로토콜 |
| **RTP** | Real-time Transport Protocol. 실시간 음성/영상 데이터 전송 프로토콜 |
| **SRTP** | Secure RTP. 암호화된 RTP |
| **PBX** | Private Branch Exchange. 기업용 전화 교환기 (예: Avaya, Genesys) |
| **SBC** | Session Border Controller. SIP 트래픽 보안/제어 장비 |
| **ESL** | Event Socket Library. FreeSWITCH를 외부에서 제어하는 TCP 프로토콜 |
| **FreeSWITCH** | 오픈소스 소프트웨어 기반 전화 교환 시스템 |
| **Orchestrator** | VBGW의 Tier 2. 콜 제어 + REST API + 세션 관리 담당 |
| **Bridge** | VBGW의 Tier 3. 오디오 ↔ AI 엔진 스트리밍 중계 담당 |
| **VAD** | Voice Activity Detection. 음성 구간을 감지하는 기술 |
| **Silero VAD** | ONNX 기반 딥러닝 VAD 모델 (v4) |
| **gRPC** | Google의 고성능 RPC 프레임워크. AI 엔진과 양방향 스트리밍에 사용 |
| **protobuf** | Protocol Buffers. gRPC 메시지 직렬화 포맷 |
| **DTMF** | Dual-Tone Multi-Frequency. 전화 키패드 톤 신호 (0-9, *, #) |
| **IVR** | Interactive Voice Response. 음성 자동 안내 시스템 ("1번을 누르세요") |
| **CDR** | Call Detail Record. 통화 상세 기록 (시작/종료 시간, 상태 등) |
| **TLS** | Transport Layer Security. 통신 암호화 |
| **ACL** | Access Control List. 접근 제어 목록 (IP 허용/차단) |
| **Barge-in** | 통화 중 끼어들기 (AI 응답 중 사용자가 말을 시작하면 AI 중단) |
| **Sofia** | FreeSWITCH의 SIP 모듈 이름 (mod_sofia) |
| **G.711** | 전화 기본 코덱 (PCMU/PCMA). 8kHz, 64kbps |
| **Prometheus** | 오픈소스 모니터링 시스템. 메트릭 수집 + 알람 |
| **SIPp** | SIP 프로토콜 테스트/부하 생성 도구 |
| **Healthcheck** | 서비스 정상 동작 여부를 확인하는 API |
| **Graceful Shutdown** | 진행 중인 통화를 완료한 후 서비스를 안전하게 종료하는 방식 |
| **Rate Limit** | 단위 시간당 API 요청 수 제한 (DDoS 방어) |

---

## 전체 테스트 흐름 요약

```
[Part 1] 환경 준비: Docker, Git, (Go, SIPp) 설치
    ↓
[Part 2] 프로젝트 설치: clone → .env 설정 → 디렉토리 생성
    ↓
[Part 3] 서비스 시작: docker compose up -d --build
    ↓
[Part 4] 정상 확인: /live → /ready → /health → Sofia → Metrics
    ↓
[Part 5] API 테스트: 콜 생성, 에러 케이스, Rate Limit
    ↓
[Part 6] 유닛 테스트: go test ./... (133개)
    ↓
[Part 7] SIP 테스트: SIPp로 실제 SIP 통화 시뮬레이션
    ↓
[Part 8] 부하 테스트: 100 동시콜, TC-01~TC-13
    ↓
[Part 9] 디버깅: 로그 확인, 에러 해결
    ↓
[Part 10] 정리: docker compose down
```
