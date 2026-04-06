# VoiceBot Gateway 초보자 사용자 매뉴얼 (한국어)

이 문서는 **개발/운영 경험이 많지 않은 사용자도 그대로 따라 해서 VBGW를 실행하고 테스트할 수 있도록** 만든 실전 가이드입니다.

- 대상 독자: SIP, gRPC, C++ 프로젝트가 익숙하지 않은 입문자
- 목표: 로컬 실행, 통화 테스트, API 제어, 장애 대응, 운영 전 점검까지 한 번에 이해
- 기준 프로젝트 루트: `vbgw/`

---

## 1. VBGW가 무엇인지 먼저 이해하기

VBGW(VoiceBot Gateway)는 전화망과 AI 엔진 사이에서 실시간 음성을 중계하는 서버입니다.

1. 전화(PBX/SBC/소프트폰)로 SIP 통화가 들어옵니다.
2. VBGW가 RTP 음성을 받아서 AI 엔진으로 gRPC 스트리밍 전송합니다.
3. AI 응답(TTS/STT/제어 신호)을 받아서 다시 통화 상대에게 음성으로 전달합니다.
4. 필요하면 DTMF, 전환(REFER), 녹취, 콜 브리지까지 제어합니다.

핵심 포인트:

1. SIP는 "전화 연결 제어"입니다.
2. RTP는 "실제 음성 데이터"입니다.
3. gRPC는 "AI와 실시간 데이터 교환"입니다.

---

## 2. 가장 빠른 10분 시작 (진짜 초보자용)

아래 6단계만 복사/실행하면 로컬에서 기본 동작까지 확인할 수 있습니다.

### 2.1 의존성 설치 (macOS)

```bash
brew update
brew install cmake pjproject grpc protobuf openssl spdlog boost onnxruntime speexdsp
```

### 2.2 VAD 모델 확인

```bash
ls models/silero_vad.onnx
```

파일이 없다면 `models/silero_vad.onnx`를 준비해야 합니다.

### 2.3 빌드

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

### 2.4 로컬 환경 변수 로드

```bash
set -a
source config/.env.local
set +a
```

### 2.5 AI 모의 서버 실행 (터미널 A)

```bash
cd src/emulator
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
python mock_server.py
```

### 2.6 VBGW 실행 + 테스트 (터미널 B)

```bash
cd /Users/kchul199/Desktop/project/antigravity_project/vbgw
set -a
source config/.env.local
set +a
./build/vbgw
```

다른 터미널(C)에서 자동 테스트:

```bash
./test.sh mock null
```

---

## 3. 실행 전에 꼭 알아야 할 폴더 구조

`vbgw/` 기준 주요 경로:

1. `src/main.cpp`: 프로그램 진입점
2. `src/engine/`: SIP 콜 제어
3. `src/ai/`: gRPC AI 통신
4. `src/api/HttpServer.cpp`: 관리 API(`/health`, `/metrics`, `/api/v1/*`)
5. `config/.env.example`: 전체 설정 템플릿
6. `config/.env.local`: 로컬 테스트 기본 설정
7. `scripts/e2e_outbound_null_audio.sh`: Outbound E2E 자동화
8. `docs/testing.md`: 테스트 중심 문서
9. `docs/api_spec.md`: 메시지/제어 API 요약

---

## 4. 환경변수 설정을 아주 쉽게 이해하기

초보자는 모든 변수를 한 번에 이해하려고 하지 말고 아래 4개 그룹부터 잡으면 됩니다.

### 4.1 필수 1군 (먼저 확인)

1. `SIP_PORT`: SIP 수신 포트 (기본 5060)
2. `AI_ENGINE_ADDR`: AI 서버 주소 (예: `localhost:50051`)
3. `HTTP_PORT`: 관리 API 포트 (예: `8080`)
4. `ADMIN_API_KEY`: 제어 API 인증키

### 4.2 품질/성능 2군

1. `MAX_CONCURRENT_CALLS`: 동시 콜 수 제한
2. `TTS_BUFFER_SECS`: TTS 버퍼 길이
3. `RTP_PORT_MIN`, `RTP_PORT_MAX`: RTP 포트 범위
4. `JB_*`: 지터버퍼 파라미터

### 4.3 보안 3군 (운영 필수)

1. `SIP_USE_TLS`, `SIP_TRANSPORT_TLS_ENABLE`
2. `SRTP_ENABLE`, `SRTP_MANDATORY`
3. `GRPC_USE_TLS`, `GRPC_TLS_*`
4. 강한 `ADMIN_API_KEY`

### 4.4 고급 4군 (NAT/세션제어/미디어)

1. NAT: `SIP_STUN_*`, `SIP_ICE_ENABLE`, `SIP_TURN_*`, `SIP_NAT_*`
2. 세션제어: `SIP_PRACK_MODE`, `SIP_SESSION_TIMER_MODE`, `SIP_TIMER_*`
3. RTP/RTCP: `RTP_STREAM_KEEPALIVE_ENABLE`, `RTP_RTCP_MUX_ENABLE`, `RTP_RTCP_XR_ENABLE`, `RTP_RTCP_FB_NACK_ENABLE`
4. 녹취: `CALL_RECORDING_ENABLE`, `CALL_RECORDING_DIR`

---

## 5. 로컬에서 통화 테스트하는 3가지 방법

### 5.1 방법 A: 가장 쉬운 자동 테스트

```bash
./test.sh mock null
```

이 방법은 음성 장치 없이 연결성과 스트리밍 파이프라인을 빠르게 확인합니다.

### 5.2 방법 B: 실제 발화 테스트

```bash
./test.sh mock voice
```

마이크/스피커를 사용하므로 OS 권한과 오디오 장치 상태가 중요합니다.

### 5.3 방법 C: 운영 시나리오에 가까운 Outbound E2E

```bash
./scripts/e2e_outbound_null_audio.sh config/.env.local
```

성공 기준:

1. 스크립트 마지막에 `PASS: Outbound API E2E scenario completed successfully.` 출력

주의:

1. 일부 제한 환경에서는 `PJMEDIA_EAUD_INIT`으로 `SKIP`될 수 있습니다.
2. 강제 실패 모드가 필요하면 `REQUIRE_MEDIA=1` 또는 `REQUIRE_E2E=1`을 사용합니다.

---

## 6. 관리 API를 실제로 써보기 (curl 실습)

기본 전제:

```bash
export ADMIN_API_KEY=changeme-admin-key
export BASE=http://127.0.0.1:8080
```

### 6.1 Outbound 콜 생성

```bash
curl -s -X POST \
  -H "X-Admin-Key: ${ADMIN_API_KEY}" \
  -H "Content-Type: application/json" \
  -d '{"target_uri":"sip:callee@127.0.0.1:15060"}' \
  ${BASE}/api/v1/calls
```

응답에서 `call_id`를 확보하세요.

### 6.2 DTMF 전송

```bash
curl -s -X POST \
  -H "X-Admin-Key: ${ADMIN_API_KEY}" \
  -H "Content-Type: application/json" \
  -d '{"digits":"123#","target":"peer"}' \
  ${BASE}/api/v1/calls/<CALL_ID>/dtmf
```

`target` 값:

1. `peer`: SIP 상대방으로 DTMF 전송
2. `ai`: AI 스트림으로 DTMF 전송
3. `both`: 둘 다 전송

### 6.3 블라인드 전환 (REFER)

```bash
curl -s -X POST \
  -H "X-Admin-Key: ${ADMIN_API_KEY}" \
  -H "Content-Type: application/json" \
  -d '{"target_uri":"sip:1002@127.0.0.1"}' \
  ${BASE}/api/v1/calls/<CALL_ID>/transfer
```

### 6.4 통화 녹취 시작/중지

시작:

```bash
curl -s -X POST \
  -H "X-Admin-Key: ${ADMIN_API_KEY}" \
  -H "Content-Type: application/json" \
  -d '{"file_path":"recordings/manual_test.wav"}' \
  ${BASE}/api/v1/calls/<CALL_ID>/record/start
```

중지:

```bash
curl -s -X POST \
  -H "X-Admin-Key: ${ADMIN_API_KEY}" \
  ${BASE}/api/v1/calls/<CALL_ID>/record/stop
```

### 6.5 통화 통계 조회

```bash
curl -s \
  -H "X-Admin-Key: ${ADMIN_API_KEY}" \
  ${BASE}/api/v1/calls/<CALL_ID>/stats
```

확인 포인트:

1. `rx_packets`, `tx_packets`
2. `rx_lost`, `rx_discard`
3. `rx_jitter_mean_usec`, `rtt_mean_usec`
4. `jbuf_*`

### 6.6 두 통화 브리지 연결/해제

브리지 연결:

```bash
curl -s -X POST \
  -H "X-Admin-Key: ${ADMIN_API_KEY}" \
  -H "Content-Type: application/json" \
  -d '{"call_a":101,"call_b":102}' \
  ${BASE}/api/v1/calls/bridge
```

브리지 해제:

```bash
curl -s -X POST \
  -H "X-Admin-Key: ${ADMIN_API_KEY}" \
  -H "Content-Type: application/json" \
  -d '{"call_a":101,"call_b":102}' \
  ${BASE}/api/v1/calls/unbridge
```

---

## 7. 상태 점검 (운영자가 매일 보는 항목)

### 7.1 Liveness/Readiness/Health

```bash
curl -s http://127.0.0.1:8080/live
curl -s http://127.0.0.1:8080/ready
curl -s http://127.0.0.1:8080/health
```

의미:

1. `/live`: 프로세스 살아있는지
2. `/ready`: SIP/gRPC 준비 상태
3. `/health`: 상세 상태(JSON)

### 7.2 Prometheus 메트릭

```bash
curl -s http://127.0.0.1:8080/metrics
```

초보자 기준 핵심 메트릭:

1. `vbgw_active_calls`
2. `vbgw_sip_registered`
3. `vbgw_grpc_stream_errors_total`
4. `vbgw_grpc_dropped_frames_total`
5. `vbgw_rtp_rx_packets_total`, `vbgw_rtp_rx_lost_total`
6. `vbgw_rtp_rx_jitter_usec_mean`, `vbgw_rtp_rtt_usec_mean`
7. `vbgw_recording_active_calls`

---

## 8. 로그 보는 법 (문제 해결의 80%)

### 8.1 콘솔 로그

`./build/vbgw` 실행 중 터미널에 바로 출력됩니다.

### 8.2 파일 로그

`LOG_DIR` 설정 시 해당 경로에 로그가 생성됩니다.

예시:

```bash
tail -f logs/vbgw.log
```

### 8.3 자주 보는 로그 키워드

1. `Incoming SIP call`
2. `AI Stream Session started`
3. `Barge-In`
4. `Outbound SIP call initiated`
5. `transfer`
6. `recording started`
7. `PJMEDIA_EAUD_INIT`

---

## 9. 초보자가 자주 겪는 문제와 해결 순서

### 9.1 문제: vbgw가 시작 직후 종료됨

점검 순서:

1. `./build/vbgw` 직접 실행해 에러 메시지 확인
2. `Audio subsystem not initialized (PJMEDIA_EAUD_INIT)`이면 null-audio 모드(`PJSIP_NULL_AUDIO=1`)로 재시도
3. `SIP transport` 에러면 포트 점유 확인 (`lsof -i :5060`)

### 9.2 문제: Outbound API가 401/403

점검 순서:

1. 요청 헤더에 `X-Admin-Key`가 있는지
2. 키 값이 `.env`의 `ADMIN_API_KEY`와 같은지
3. 공백/개행 문자 포함 여부

### 9.3 문제: Outbound API가 429

원인:

1. rate-limit 초과

해결:

1. `ADMIN_API_RATE_LIMIT_RPS`, `ADMIN_API_RATE_LIMIT_BURST` 조정
2. 호출 속도 줄이기

### 9.4 문제: 통화는 되는데 AI 응답이 없음

점검 순서:

1. `mock_server.py` 또는 AI 서버가 실제 실행 중인지 (`lsof -i tcp:50051`)
2. `AI_ENGINE_ADDR`가 맞는지
3. gRPC TLS 설정(`GRPC_USE_TLS`, 인증서 경로) 일치 여부

### 9.5 문제: 상대방 음성이 끊기거나 지연됨

점검 항목:

1. `JB_*` 값이 너무 공격적이지 않은지
2. `RTP_PORT_MIN/MAX` 방화벽 허용 여부
3. 네트워크 품질
4. `/metrics`에서 `vbgw_rtp_rx_lost_total`, `vbgw_rtp_rx_jitter_usec_mean` 증가 여부

---

## 10. 운영 배포 전 체크리스트 (실수 방지용)

배포 전에 아래를 반드시 모두 확인하세요.

1. 프로파일이 production인지 (`VBGW_PROFILE=production`)
2. `SIP_TRANSPORT_TLS_ENABLE=1` 또는 `SIP_USE_TLS=1`
3. `SRTP_ENABLE=1`, `SRTP_MANDATORY=1`
4. `GRPC_USE_TLS=1`
5. TLS 인증서 파일 경로가 실제 존재하는지
6. `ADMIN_API_KEY`가 강한 값인지
7. `HTTP_PORT`와 `SIP_PORT`가 충돌하지 않는지
8. `validate_prod_env.sh` 통과 여부

검증 명령:

```bash
VALIDATE_PROFILE=production REQUIRE_PRODUCTION_PROFILE=1 ./scripts/validate_prod_env.sh .env
```

---

## 11. 운영 시작/중지 표준 절차

### 11.1 시작

1. 환경변수 로드
2. AI 백엔드 준비 확인
3. `./build/vbgw` 실행
4. `/live`, `/ready`, `/health` 확인

### 11.2 중지

1. 실행 터미널에서 `Ctrl+C`
2. 로그에 graceful shutdown 단계가 정상 완료됐는지 확인

---

## 12. 초보자용 명령어 모음 (치트시트)

빌드:

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

테스트:

```bash
ctest --test-dir build --output-on-failure
bash tests/test_validate_prod_env.sh
./test.sh mock null
./scripts/e2e_outbound_null_audio.sh config/.env.local
```

상태 확인:

```bash
curl -s http://127.0.0.1:8080/live
curl -s http://127.0.0.1:8080/ready
curl -s http://127.0.0.1:8080/health
curl -s http://127.0.0.1:8080/metrics
```

---

## 13. 용어 사전 (처음 보는 분용)

1. SIP: 전화 연결/종료/전환 같은 제어 프로토콜
2. RTP: 실제 음성 데이터 전송 프로토콜
3. PBX/SBC: 기업 전화망의 교환/경계 장비
4. STT: 음성 -> 텍스트
5. TTS: 텍스트 -> 음성
6. VAD: 사용자가 말하는지 감지
7. Barge-in: 봇이 말하는 중 사용자 발화가 들어오면 재생 중단
8. REFER: 통화를 다른 대상에게 전환 요청
9. PRACK: 1xx 응답 신뢰 전달
10. Session Timer: 장시간 통화 세션 유지 확인

---

## 14. 권장 학습 순서

처음인 경우 아래 순서대로만 진행하면 됩니다.

1. `2장(10분 시작)` 실행
2. `5장(테스트 3가지)` 중 A와 C 수행
3. `6장(API 실습)`에서 Outbound + Stats + DTMF 실행
4. `7장(상태 점검)`에서 `/health`, `/metrics` 읽기
5. 마지막으로 `10장(운영 체크리스트)` 점검

이 순서를 따라오면 “실행은 되는데 왜 되는지 모르는 상태”를 빠르게 벗어날 수 있습니다.
