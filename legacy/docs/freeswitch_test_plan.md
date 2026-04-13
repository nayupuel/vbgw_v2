# FreeSWITCH Migration Test Plan

> **Version**: v1.0.0 | 2026-04-07 | QA  
> **Parent**: `freeswitch_migration_v2.md`  
> **Based on**: `test_cases.md` (TC-01 ~ TC-08)

---

## Overview

기존 C++ vbgw의 8개 테스트 케이스를 FreeSWITCH 3-tier 스택에서 재검증합니다.

### Pass Criteria (전체)

- TC-01 ~ TC-08: **전원 PASS**
- Critical failure 0건
- SLO 이탈 0건

---

## TC-01: SIP Call Setup / Teardown

### Purpose
SIP INVITE→180→200 OK→BYE 시퀀스 정상 동작 + 메모리 누수 없음 확인

### Prerequisites
- Docker Compose 전체 서비스 가동
- SIPp 설치 (`apt install sipp`)

### Steps

```bash
# 1. SIPp로 1000회 콜 셋업/해제 (동시 10콜, 각 5초 유지)
sipp -sn uac -d 5000 -l 10 -m 1000 \
     -r 10 -rp 1000 \
     <freeswitch_ip>:5060

# 2. Orchestrator 로그 확인
docker compose logs orchestrator | grep "CHANNEL_CREATE"  # 1000건
docker compose logs orchestrator | grep "CHANNEL_HANGUP"  # 1000건

# 3. 세션 카운터 확인 (모든 콜 종료 후 0이어야 함)
curl -s http://localhost:8080/health | jq '.active_calls'
# Expected: 0

# 4. 메모리 누수 확인 (Go pprof)
curl -s http://localhost:8080/debug/pprof/heap > heap_after.prof
go tool pprof heap_after.prof
# Expected: heap 선형 증가 없음
```

### Pass Criteria
- SIPp: 1000/1000 calls completed (0 failures)
- Orchestrator: CHANNEL_CREATE 1000건, CHANNEL_HANGUP 1000건
- active_calls = 0 (모든 콜 종료 후)
- pprof heap: 선형 증가 없음

---

## TC-02: RTP Media Quality & DSP

### Purpose
G.711 → 16kHz PCM 트랜스코딩 + AGC 적용 + Bridge 수신 정상 확인

### Steps

```bash
# 1. SIPp에 WAV 오디오 파일 주입
sipp -sn uac -d 10000 -l 1 -m 1 \
     -inf audio_test.csv \
     <freeswitch_ip>:5060

# 2. Bridge 로그에서 PCM 프레임 수신 확인
docker compose logs bridge | grep "WS frame received"
# Expected: size=640 (320 samples × 2 bytes) per 20ms

# 3. FS AGC 적용 확인
docker compose exec freeswitch fs_cli -x "show channels" | grep agc
# Expected: agc=1

# 4. RTP 통계 확인 (콜 진행 중)
CALL_ID=$(curl -s http://localhost:8080/health | jq -r '.calls[0].call_id')
curl -s http://localhost:8080/api/v1/calls/$CALL_ID/stats | jq
# Expected: rtp_packets > 0, rtp_loss = 0
```

### Pass Criteria
- Bridge: 640-byte L16 PCM 프레임 정상 수신 (20ms 간격)
- FS: AGC 활성화 확인
- RTP: 패킷 손실 0%, 수신 패킷 > 0

---

## TC-03: Silero VAD & gRPC AI Streaming

### Purpose
VAD 발화 감지 정확도 + gRPC AudioChunk 전송 검증

### Prerequisites
- Mock AI 서버 가동 (`src/emulator/mock_server.py`)

### Steps

```bash
# 1. Mock AI 서버 시작
cd src/emulator && python mock_server.py &

# 2. SIPp로 음성 파일 포함 콜 발신
sipp -sn uac -d 10000 -l 1 -m 1 \
     -inf voice_sample.csv \
     <freeswitch_ip>:5060

# 3. Bridge VAD 로그 확인
docker compose logs bridge | grep "VAD inference"
# Expected: is_speaking=true/false 전이 확인

# 4. Mock AI 수신 카운터 확인
curl -s http://localhost:50051/debug/stats
# Expected: audio_chunks_received > 0, session_id 일치

# 5. VAD 메트릭 확인
curl -s http://localhost:8080/metrics | grep vbgw_vad_speech_events_total
# Expected: > 0
```

### Pass Criteria
- VAD: is_speaking true↔false 전이 발생
- Mock AI: AudioChunk 수신 바이트 > 0
- Session ID: Orchestrator UUID와 gRPC session_id 일치

---

## TC-04: Barge-in / TTS Buffer Flush

### Purpose
AI `clear_buffer=true` 수신 → TTS 즉시 중단 → uuid_break 실행 확인

### Steps

```bash
# 1. Mock AI를 barge-in 모드로 설정 (3초 후 clear_buffer=true 송신)
cd src/emulator && python mock_server.py --barge-in-after 3

# 2. 콜 발신
sipp -sn uac -d 20000 -l 1 -m 1 <freeswitch_ip>:5060

# 3. Bridge 로그: TTS 버퍼 드레인 확인
docker compose logs bridge | grep "TTS buffer drained"
# Expected: frames_discarded >= 1

# 4. Orchestrator 로그: uuid_break 실행 확인
docker compose logs orchestrator | grep "uuid_break"
# Expected: uuid_break sent for <uuid>

# 5. FS 로그: break 실행 확인
docker compose exec freeswitch fs_cli -x "log" | grep "break"
# Expected: break executed

# 6. 타이밍 측정
# Bridge 로그 타임스탬프: clear_buffer 수신 → uuid_break 완료
# Expected: P95 ≤ 200ms (목표: 100ms)
```

### Pass Criteria
- TTS 버퍼 드레인: frames_discarded ≥ 1
- uuid_break 실행 확인
- 타이밍: clear_buffer → FS 재생 중단 ≤ 200ms

---

## TC-05: IVR State Machine & DTMF Routing

### Purpose
IVR 5-state FSM 전이 정상 + DTMF 라우팅 정확 + 교착 없음

### Steps

```bash
# 1. SIPp에서 DTMF 시퀀스 송신
# Scenario: INVITE → answer → DTMF '1' → DTMF '2' → DTMF '0' → DTMF '*' → DTMF '#'
sipp -sf dtmf_scenario.xml -l 1 -m 1 <freeswitch_ip>:5060

# dtmf_scenario.xml sends RFC 2833 DTMF events in sequence

# 2. Orchestrator IVR 로그 확인
docker compose logs orchestrator | grep "IVR"
# Expected sequence:
#   IVR state: IDLE → MENU
#   IVR state: MENU → AI_CHAT (DTMF '1')
#   IVR: forward DTMF '2' to AI
#   IVR state: AI_CHAT → TRANSFER (DTMF '0')
#   IVR state: TRANSFER → MENU (DTMF '*')
#   IVR state: MENU → DISCONNECT (DTMF '#')

# 3. 교착 확인 (goroutine dump)
kill -SIGUSR1 $(pidof orchestrator)
docker compose logs orchestrator | grep "goroutine"
# Expected: IVR goroutine이 "select" 대기 상태 (교착 아님)

# 4. DTMF→AI 전달 확인
docker compose logs bridge | grep "DTMF forwarded"
# Expected: digit='2' forwarded to gRPC
```

### Pass Criteria
- 5개 상태 전이 모두 정상
- DTMF '2' → AI 전달 확인
- Goroutine 교착 0건

---

## TC-06: Call Bridge & AI Pause

### Purpose
1:1 브릿지 시 AI 스트리밍 중지 + 언브릿지 시 재개 확인

### Steps

```bash
# 1. 콜 A 발신
CALL_A=$(curl -s -X POST http://localhost:8080/api/v1/calls \
  -H "X-Admin-Key: $ADMIN_API_KEY" \
  -d '{"target_uri":"sip:1001@pbx"}' | jq -r '.call_id')

# 2. 콜 B 발신
CALL_B=$(curl -s -X POST http://localhost:8080/api/v1/calls \
  -H "X-Admin-Key: $ADMIN_API_KEY" \
  -d '{"target_uri":"sip:1002@pbx"}' | jq -r '.call_id')

# 3. 브릿지
curl -s -X POST http://localhost:8080/api/v1/calls/bridge \
  -H "X-Admin-Key: $ADMIN_API_KEY" \
  -d "{\"call_id_1\":$CALL_A,\"call_id_2\":$CALL_B}"

# 4. Bridge 로그: AI 전송 중지 확인
docker compose logs bridge | grep "AI gRPC send PAUSED"
# Expected: session for CALL_A paused

# 5. Mock AI 수신 모니터: CALL_A AudioChunk 중단
# Expected: 0 bytes from CALL_A after bridge

# 6. 언브릿지
curl -s -X POST http://localhost:8080/api/v1/calls/unbridge \
  -H "X-Admin-Key: $ADMIN_API_KEY" \
  -d "{\"call_id_1\":$CALL_A,\"call_id_2\":$CALL_B}"

# 7. Bridge 로그: AI 전송 재개 확인
docker compose logs bridge | grep "AI gRPC send RESUMED"
# Expected: session for CALL_A resumed
```

### Pass Criteria
- Bridge 후: CALL_A의 AI AudioChunk 수신 0 bytes
- Unbridge 후: CALL_A의 AI AudioChunk 수신 재개
- FS: uuid_bridge/uuid_transfer 정상 실행

---

## TC-07: Recording Quota & Retention

### Purpose
녹음 파일 age(30일) + quota(MB) 기반 자동 정리 확인

### Steps

```bash
# 1. 테스트 더미 파일 생성 (30일 이전 날짜)
for i in $(seq 1 10); do
  touch -d "31 days ago" recordings/old_recording_$i.wav
  dd if=/dev/urandom of=recordings/old_recording_$i.wav bs=1M count=1
done

# 2. 현재 파일 확인
ls -la recordings/
# Expected: 10 old files

# 3. Orchestrator 녹음 정리 트리거 (즉시 실행 엔드포인트 또는 1시간 대기)
# Option A: 테스트용 즉시 트리거
curl -X POST http://localhost:8080/internal/cleanup-now

# Option B: 1시간 대기 후 자동 실행

# 4. 정리 결과 확인
docker compose logs orchestrator | grep "removed old recording"
# Expected: 10건 삭제 로그

# 5. 파일시스템 확인
ls -la recordings/old_recording_*.wav
# Expected: 파일 없음 (all deleted)

# 6. Quota 테스트 (RECORDING_MAX_MB=10으로 설정)
# 11MB 파일 생성 후 정리 실행
dd if=/dev/urandom of=recordings/big_file.wav bs=1M count=11
# Trigger cleanup → oldest files removed until under quota
```

### Pass Criteria
- 30일 초과 파일 전량 삭제
- Quota 초과 시 oldest-first 삭제
- 삭제 로그 정확

---

## TC-08: Graceful Shutdown (5-Stage)

### Purpose
SIGINT → 5단계 순서 정상 + Exit 0 + Segfault/Panic 없음

### Steps

```bash
# 1. 동시 3개 콜 유지 상태 생성
for i in 1 2 3; do
  sipp -sn uac -d 60000 -l 1 -m 1 <freeswitch_ip>:5060 &
done
sleep 3

# 2. 활성 콜 확인
curl -s http://localhost:8080/health | jq '.active_calls'
# Expected: 3

# 3. Graceful shutdown 시작
docker compose exec orchestrator kill -INT 1

# 4. 로그 순서 확인
docker compose logs orchestrator | grep "Shutdown"
# Expected (순서대로):
#   [Shutdown 1/5] HTTP server: rejecting new requests
#   [Shutdown 2/5] ESL: fsctl pause sent
#   [Shutdown 3/5] Waiting for 3 sessions to drain (timeout=30s)
#   [Shutdown 4/5] Bridge gRPC streams closed
#   [Shutdown 5/5] ESL connection closed

# 5. Exit 코드 확인
echo $?
# Expected: 0

# 6. Panic/Segfault 확인
docker compose logs orchestrator | grep -i "panic\|fatal\|segfault"
# Expected: 0 matches

# 7. FS 콜 상태 확인 (BYE 정상 전송)
docker compose exec freeswitch fs_cli -x "show calls"
# Expected: 0 total (모든 콜 BYE 처리 완료)
```

### Pass Criteria
- 5단계 로그 순서 정확
- Exit code = 0
- Panic/Segfault 0건
- 기존 3개 콜 BYE 정상 처리

---

## Phase 3 부하 테스트 절차

### Load Test: 100 Concurrent Calls (30 minutes)

```bash
# SIPp 부하 생성
sipp -sn uac \
  -d 300000 \       # 각 콜 5분 유지
  -l 100 \          # 동시 100콜
  -m 600 \          # 총 600콜 (5분씩 100콜 = 30분 순환)
  -r 2 -rp 1000 \   # 초당 2콜 생성
  <freeswitch_ip>:5060

# 모니터링 (별도 터미널)
watch -n 5 'curl -s http://localhost:8080/metrics | grep -E "active_calls|dropped_frames|stream_errors"'
```

### SLO Verification

| Metric | Target | Command |
|--------|--------|---------|
| /health uptime | 99.95% | `while true; do curl -s -o /dev/null -w "%{http_code}\n" http://localhost:8080/health; sleep 1; done \| sort \| uniq -c` |
| API P95 latency | ≤ 300ms | `hey -n 1000 -c 10 -H "X-Admin-Key:$ADMIN_API_KEY" -m POST -d '{"target_uri":"sip:test@pbx"}' http://localhost:8080/api/v1/calls` |
| Audio queue drop | ≤ 0.1% | `curl -s http://localhost:8080/metrics \| grep dropped_frames` |
| gRPC stream errors | ≤ 0.1% | `curl -s http://localhost:8080/metrics \| grep stream_errors` |

### Memory Leak Detection (72h)

```bash
# 시작 시 기준선
curl -s http://localhost:8080/debug/pprof/heap > heap_0h.prof

# 24h 후
curl -s http://localhost:8080/debug/pprof/heap > heap_24h.prof

# 48h 후
curl -s http://localhost:8080/debug/pprof/heap > heap_48h.prof

# 72h 후
curl -s http://localhost:8080/debug/pprof/heap > heap_72h.prof

# 비교
go tool pprof -diff_base heap_0h.prof heap_72h.prof
# Expected: inuse_space 선형 증가 없음
```

---

## Test Matrix Summary

| TC | Name | Phase 1 | Phase 2 | Phase 3 |
|----|------|---------|---------|---------|
| TC-01 | SIP Setup/Teardown | Manual | Automated | Load (1000 calls) |
| TC-02 | RTP Quality | Manual | Automated | Load (100 concurrent) |
| TC-03 | VAD + gRPC | Manual | Automated | Load (100 concurrent) |
| TC-04 | Barge-in | Manual | Automated | Timing P95 |
| TC-05 | IVR/DTMF | - | Automated | Stress (rapid DTMF) |
| TC-06 | Bridge | - | Automated | Load (50 bridges) |
| TC-07 | Recording | - | Automated | Quota stress |
| TC-08 | Shutdown | - | Automated | Under load |
