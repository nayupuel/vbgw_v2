# 유저 테스트 가이드

VoiceBot Gateway 로컬 통합 테스트 절차서입니다.

---

## 테스트 환경 구성

```
[pjsua CLI] ─── SIP:5060 ──▶ [vbgw] ─── gRPC:50051 ──▶ [mock_server / emulator]
 (SIP 발신)                 (게이트웨이)                 (AI 서버 목)
```

### 전제조건 확인

| 항목 | 위치 | 확인 명령 |
|------|------|----------|
| C++ 바이너리 | `build/vbgw` | `ls build/vbgw` |
| VAD 모델 | `models/silero_vad.onnx` | `ls models/silero_vad.onnx` |
| Python venv | `src/emulator/venv/` | `ls src/emulator/venv` |
| pjsua CLI | 시스템 PATH | `which pjsua` |

---

## 방법 1 — 자동 스크립트 (권장)

프로젝트 루트에서 한 번에 실행:

```bash
# 빠른 연결 검증 (무음, 가장 빠름)
./test.sh mock null

# 실제 음성 테스트 (마이크/스피커 필요)
./test.sh mock voice

# 고급: WAV 파일 재생 + 오디오 캡처
./test.sh emul voice
```

### 스크립트 동작 순서

1. `mock_server.py` (또는 `emulator.py`) 를 백그라운드로 시작
2. `vbgw` 를 `config/.env.local` 설정으로 시작
3. `pjsua` 로 `sip:voicebot@127.0.0.1:5060` 호출

---

## 방법 2 — 터미널 3개 수동 실행

### Terminal 1 — AI 목 서버

```bash
cd src/emulator
source venv/bin/activate
python mock_server.py
```

**기대 출력:**
```
🚀 Python STT/TTS Emulator running on localhost:50051...
Waiting for VBGW C++ Gateway calls...
```

### Terminal 2 — VoiceBot Gateway

```bash
# 프로젝트 루트에서 실행 (모델 상대경로 기준)
set -a; source config/.env.local; set +a
./build/vbgw
```

**기대 출력:**
```
[2026-xx-xx] [info] Starting AI Voicebot Gateway (PJSUA2)... [log_level=debug]
[2026-xx-xx] [info] [Endpoint] PJSIP initialized [pjsip_log_level=4]
[2026-xx-xx] [info] [Endpoint] SIP UDP transport started on port 5060
[2026-xx-xx] [error] [Call] gRPC using INSECURE channel — GRPC_USE_TLS=1 is REQUIRED for production
[2026-xx-xx] [info] [VBGW] Local Mode Enabled (No PBX). Direct IP calls: sip:voicebot@127.0.0.1
[2026-xx-xx] [info] Press Ctrl+C to stop the gateway gracefully.
```

> ⚠️ `INSECURE channel` 에러는 로컬 테스트에서 정상입니다. `GRPC_USE_TLS=0` 설정 결과입니다.

### Terminal 3 — SIP 콜 발신

```bash
# 옵션 A: Null Audio (연결만 검증, 빠름)
pjsua --null-audio --no-tcp --local-port=15060 --log-level=1 sip:voicebot@127.0.0.1:5060

# 옵션 B: 실제 음성 (마이크/스피커 사용)
pjsua --no-tcp --local-port=15060 --log-level=1 sip:voicebot@127.0.0.1:5060
```

**pjsua 인터랙티브 명령:**

| 키 | 동작 |
|----|------|
| `h` + Enter | 현재 통화 종료 (BYE) |
| `q` + Enter | pjsua 종료 |
| `m` + Enter | 통화 음소거 토글 |

---

## 테스트 시나리오별 기대 동작

### 시나리오 1 — Null Audio (연결 검증)

```
[pjsua] INVITE ──▶ [vbgw] ──▶ 180 Ringing
                            ──▶ 200 OK
[pjsua] ACK    ──▶ [vbgw]
[vbgw] gRPC StreamSession 시작 ──▶ [mock_server]
[vbgw] AudioChunk (is_speaking=false) ──▶ [mock_server] (무한 반복)
```

**vbgw 로그에서 확인할 것:**
```
[info] [Account] Incoming SIP call, Call-ID: 1
[info] [Account] Sent 180 Ringing for Call-ID: 1
[info] [Account] Sent 200 OK for Call-ID: 1
[info] [Call] Connecting to AI Engine at: localhost:50051
[error] [Call] gRPC using INSECURE channel ...    ← 로컬 테스트 정상
[info] [gRPC] AI Stream Session started for: 1
[info] [Call] AI Media Port connected. RTP Stream converting to PCM.
```

**mock_server 로그에서 확인할 것:**
```
====================================
[Gateway] New SIP Call connected to AI.
```

### 시나리오 2 — 실제 음성 (Barge-in + TTS 재생)

1. **통화 연결** — vbgw가 200 OK 응답
2. **침묵 구간** — VAD `is_speaking=false` 지속 전송
3. **발화 시작** — 마이크에 말하면 VAD가 `is_speaking=true` 감지
4. **Barge-in** — mock_server가 `END_OF_TURN(clear_buffer=true)` 전송 → vbgw TTS 버퍼 플러시
5. **발화 종료** — VAD `is_speaking=false` 전환
6. **STT 결과** — mock_server가 `STT_RESULT` 전송 → vbgw 로그에 출력
7. **TTS 재생** — mock_server가 beep 오디오 스트리밍 → pjsua 스피커로 재생
8. **END_OF_TURN** — 재생 완료 신호

**vbgw 로그에서 확인할 것:**
```
[info] [AI STT] User (1): 네, 고객님 요청하신 사항을 처리하겠습니다...
[warn] 🚨 [Barge-In] Flushed Gateway TTS RingBuffer! Session: 1    ← 말끊기 시
```

**mock_server 로그에서 확인할 것:**
```
🗣️ [STT/VAD] User started speaking! (VAD On -> Trigger Barge-in)
🤐 [STT/VAD] User stopped speaking. (VAD Off, Gathered XXXX bytes of voice)
🤖 [AI] Sending mock STT and TTS response...
🎵 [TTS] Streaming generated audio chunks...
✅ [AI] End of Turn Signal Sent.
```

---

## 종료 및 정리

```bash
# Terminal 3: pjsua 종료
h      # 통화 종료
q      # pjsua 종료

# Terminal 2: vbgw Graceful Shutdown
Ctrl+C

# 기대 출력:
# [Shutdown 1/3] Hanging up all active calls...
# [Shutdown 2/3] Destroying account...
# [Shutdown 3/3] Shutting down PJLIB...
# Gateway shutdown complete. Goodbye!
```

---

## 문제 해결

### "SIP transport failed" — 포트 충돌
```bash
# 5060 포트 사용 중인 프로세스 확인
lsof -i udp:5060
# → SIP_PORT=5061 으로 변경 후 재시도
```

### "gRPC connection refused"
```bash
# mock_server가 실행 중인지 확인
lsof -i tcp:50051
# → Terminal 1 먼저 실행 후 Terminal 2 시작
```

### pjsua "No audio device"
```bash
# Null Audio 모드로 전환
pjsua --null-audio --no-tcp --local-port=15060 sip:voicebot@127.0.0.1:5060
```

### VAD가 말을 감지하지 못할 때
```bash
# LOG_LEVEL=trace 로 변경하여 VAD 출력 상세 확인
LOG_LEVEL=trace ./build/vbgw
# 또는 PJSIP_LOG_LEVEL=5 로 오디오 프레임 수신 여부 확인
```

### macOS 마이크 권한 오류
- **시스템 환경설정 → 보안 및 개인 정보 보호 → 마이크** 에서 Terminal 권한 허용

---

## Sprint 2 추가 검증

### Outbound Control-plane 부하 검증

vbgw 실행 후, 별도 터미널에서:

```bash
ADMIN_API_KEY=changeme-admin-key ./scripts/soak_outbound_api.sh 100 10 sip:1001@127.0.0.1 http://127.0.0.1:8080
```

- 기대 결과: `202 Accepted` 비율이 높고, `5xx`가 없어야 함
- 관측 지표: `/metrics`의 `vbgw_grpc_queued_frames`, `vbgw_grpc_dropped_frames_total`

### Outbound API 실제 콜 E2E (Null Audio)

로컬에서 통화 장치 없이 Outbound 콜을 재현합니다.

```bash
./scripts/e2e_outbound_null_audio.sh config/.env.local
```

- 내부 동작:
  - `mock_server.py` 시작 (gRPC AI)
  - `vbgw` 시작 (`PJSIP_NULL_AUDIO=1`)
  - `pjsua` callee 시작 (`--null-audio --auto-answer=200`)
  - `/api/v1/calls` 호출 후 `call_id` 및 상태 전이 확인
- 참고:
  - 샌드박스/오디오 제한 환경에서는 `PJMEDIA_EAUD_INIT`로 `SKIP`될 수 있습니다.
  - 강제 실패 모드: `REQUIRE_MEDIA=1 ./scripts/e2e_outbound_null_audio.sh config/.env.local`
  - 네트워크/바인딩 포함 강제 모드: `REQUIRE_MEDIA=1 REQUIRE_E2E=1 ./scripts/e2e_outbound_null_audio.sh config/.env.local`

### 운영 환경 설정 사전 검증

```bash
./scripts/validate_prod_env.sh .env
```

- 기대 결과: `PASS: production env validation succeeded.`
- 운영 게이트(프로파일 강제) 권장:
  ```bash
  VALIDATE_PROFILE=production REQUIRE_PRODUCTION_PROFILE=1 ./scripts/validate_prod_env.sh .env
  ```
