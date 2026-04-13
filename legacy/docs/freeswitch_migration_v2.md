# FreeSWITCH Migration v2 — Master Design Document

> **Version**: v2.0.0 | 2026-04-07 | Architect  
> **Status**: Approved  
> **Supersedes**: `docs/freeswitch_migration_proposal.md` (v1)

---

## 1. Executive Summary

C++20/PJSIP 모놀리스 VoiceBot Gateway(vbgw)를 FreeSWITCH 기반 3-tier 아키텍처로 전환합니다.

| 항목 | As-Is (C++/PJSIP) | To-Be (FreeSWITCH) |
|------|-------------------|---------------------|
| 아키텍처 | 단일 프로세스 모놀리스 | 3-tier (FS + Orchestrator + Bridge) |
| 미디어 엔진 | PJSIP UA (수동 제어) | FreeSWITCH Core (커널 수준) |
| 비즈니스 로직 | C++20 (콜백 기반) | Go (channel 기반, 교착 원천 차단) |
| AI 연동 | gRPC 직접 (in-process) | WebSocket Bridge → gRPC |
| VAD | ONNX C++ (in-process) | ONNX Go (Bridge 내) |
| 동시 호 | 100 (수직 확장) | 2,000+ (수평 확장 가능) |
| 설정 변경 | 재빌드 필요 | XML/env 핫리로드 |

**전환 기간**: 9주 (Phase 1~4)  
**기능 보존**: 100% (16 SIP + 8 오디오 + 12 API + 5 IVR + 8 운영 = **49개 기능**)

---

## 2. To-Be Architecture

```
┌──────────────────────────────────────────────────────────────────────┐
│                         PBX / SBC                                     │
│              (UDP/TCP/TLS SIP + RTP/SRTP)                             │
└───────────────────────────┬──────────────────────────────────────────┘
                            │ SIP INVITE / BYE / REFER / DTMF
                            ▼
┌──────────────────────────────────────────────────────────────────────┐
│  Tier 1: FreeSWITCH (Media Switching Engine)                         │
│                                                                       │
│  ┌─────────────┐  ┌───────────────┐  ┌────────────────────────────┐ │
│  │  mod_sofia   │  │mod_audio_fork │  │   mod_event_socket         │ │
│  │ (SIP/TLS/   │  │(L16 PCM →     │  │  (ESL TCP 8021)            │ │
│  │  SRTP/STUN) │  │ WebSocket)    │  │                            │ │
│  └─────────────┘  └───────────────┘  └────────────────────────────┘ │
│                                                                       │
│  ┌─────────────┐  ┌───────────────┐  ┌────────────────────────────┐ │
│  │ mod_dptools  │  │  mod_record   │  │   FS Core DSP              │ │
│  │ (uuid_break, │  │ (uuid_record) │  │  (Jitter, AGC, Resample)  │ │
│  │  park, etc.) │  └───────────────┘  └────────────────────────────┘ │
│  └─────────────┘                                                      │
└──────────────────────────────┬───────────────────────────────────────┘
                               │ ESL TCP (이벤트 + API 명령)
                               ▼
┌──────────────────────────────────────────────────────────────────────┐
│  Tier 2: Go Orchestrator (Business Logic Engine)                     │
│                                                                       │
│  ┌───────────────┐  ┌─────────────┐  ┌───────────────────────────┐ │
│  │  HTTP API     │  │ ESL Client  │  │   IVR State Machine       │ │
│  │  (12 endpoints│  │ (이벤트수신 │  │  (IDLE→MENU→AI_CHAT/      │ │
│  │   + Prometheus│  │  + API 발행)│  │   TRANSFER/DISCONNECT)    │ │
│  │   + Rate Limit│  │             │  │                           │ │
│  └───────────────┘  └─────────────┘  └───────────────────────────┘ │
│                                                                       │
│  ┌───────────────┐  ┌─────────────┐  ┌───────────────────────────┐ │
│  │ Session Store │  │ CDR Logger  │  │  Recording Cleaner        │ │
│  │ (atomic CAS   │  │ (JSON CDR)  │  │  (hourly, age+quota)      │ │
│  │  100 cap)     │  │             │  │                           │ │
│  └───────────────┘  └─────────────┘  └───────────────────────────┘ │
└──────────────────────────────┬───────────────────────────────────────┘
                               │ Internal HTTP + WebSocket
                               ▼
┌──────────────────────────────────────────────────────────────────────┐
│  Tier 3: WebSocket Bridge Server (Go)                                │
│                                                                       │
│  Per-session goroutines:                                              │
│  ┌─────────────────────────────────────────────────────────────────┐ │
│  │ WS Rx → Silero VAD (ONNX) → AudioChunk → gRPC Send            │ │
│  │ gRPC Recv → AiResponse → TTS Buffer → WS Tx (→ FS playback)   │ │
│  └─────────────────────────────────────────────────────────────────┘ │
│                                                                       │
│  ┌───────────────┐  ┌─────────────────────────────────────────────┐ │
│  │  VAD Engine   │  │  Barge-in Controller                        │ │
│  │ (onnxruntime- │  │ (clear_buffer → HTTP → Orchestrator ESL)   │ │
│  │  go + silero) │  │                                             │ │
│  └───────────────┘  └─────────────────────────────────────────────┘ │
└──────────────────────────────┬───────────────────────────────────────┘
                               │ gRPC Bidirectional Streaming
                               ▼
┌──────────────────────────────────────────────────────────────────────┐
│  AI Engine (STT / NLU / TTS) — 기존 gRPC 인터페이스 유지             │
│  protos/voicebot.proto 변경 없음                                      │
└──────────────────────────────────────────────────────────────────────┘
```

---

## 3. Feature Mapping (100% Preservation)

### 3.1 SIP / Call Control (16 features)

| # | C++ Feature | FS Equivalent | Implementation |
|---|-------------|---------------|----------------|
| S-01 | Multi-transport (UDP/TCP/TLS) | mod_sofia multi-profile | `internal.xml` + `external.xml` |
| S-02 | SRTP mandatory (prod) | sofia param | `rtp-secure-media=mandatory` |
| S-03 | PBX registration + 60s retry | sofia gateway | `<gateway>` + `retry-seconds=60` |
| S-04 | Inbound 180→delay→200 | dialplan apps | `sleep($answer_delay_ms)` → `ring_ready` → `answer` |
| S-05 | Outbound call (HTTP API) | Orchestrator ESL | `bgapi originate sofia/gateway/pbx/{target} &park()` |
| S-06 | DTMF (in-band + RFC 2833) | mod_sofia built-in | `dtmf-type=rfc2833` + `start_dtmf` app |
| S-07 | Blind transfer (REFER) | ESL command | `uuid_transfer <uuid> <target> XML default` |
| S-08 | Call bridge (1:1) | ESL command | `uuid_bridge <uuid-a> <uuid-b>` |
| S-09 | Call unbridge | ESL command | `uuid_transfer <uuid> park` |
| S-10 | Session capacity 100 (TOCTOU) | Go atomic CAS | `atomic.Int64.CompareAndSwap(cur, cur+1)` |
| S-11 | UUID v4 session ID | Go library | `github.com/google/uuid` + FS uuid 매핑 |
| S-12 | NAT (STUN/TURN/ICE) | mod_sofia params | `ext-rtp-ip=stun:stun.example.com` |
| S-13 | PRACK (RFC 3262) | sofia param | `enable-100rel=true` |
| S-14 | Session Timer (RFC 4028) | sofia param | `enable-timer=true` |
| S-15 | Codec priority (Opus>G722>G711) | sofia codec prefs | `inbound-codec-prefs=opus,G722,PCMU,PCMA` |
| S-16 | Jitter buffer tuning | sofia param | `jitter-buffer-ms=100,240,500` |

### 3.2 Audio / AI Pipeline (8 features)

| # | C++ Feature | To-Be | Implementation |
|---|-------------|-------|----------------|
| A-01 | G.711 8kHz → 16kHz PCM | FS core resample | Automatic transcoding |
| A-02 | SpeexDSP denoise + AGC | FS dialplan app | `<action application="agc" data="1"/>` |
| A-03 | Silero VAD v4 (ONNX, 32ms) | WS Bridge | `onnxruntime-go` + 동일 `silero_vad.onnx` |
| A-04 | gRPC bidirectional streaming | WS Bridge | 기존 `voicebot.proto` 그대로 사용 |
| A-05 | Audio streaming (RTP→AI) | mod_audio_fork | `ws://bridge:8090/audio/{uuid}` L16 PCM |
| A-06 | Barge-in (TTS buffer flush) | Bridge→Orchestrator→ESL | `uuid_break <uuid> all` |
| A-07 | TTS ring buffer (5s, 320KB) | Go buffered channel | `chan []byte` (capacity 200 frames) |
| A-08 | DTMF→AI forwarding | ESL→Orch→Bridge→gRPC | `AudioChunk.dtmf_digit` field |

### 3.3 REST API (12 endpoints)

| # | Endpoint | Location | Internal Action |
|---|----------|----------|-----------------|
| E-01 | `GET /live` | Orchestrator | Process alive → 200 |
| E-02 | `GET /ready` | Orchestrator | ESL connected + sofia registered |
| E-03 | `GET /health` | Orchestrator | FS + Bridge + AI 3-way health |
| E-04 | `GET /metrics` | Orchestrator | `promhttp.Handler()` |
| E-05 | `POST /api/v1/calls` | Orchestrator | Atomic capacity check → ESL `bgapi originate` |
| E-06 | `POST /api/v1/calls/{id}/dtmf` | Orchestrator | ESL `uuid_send_dtmf <uuid> <digits>` |
| E-07 | `POST /api/v1/calls/{id}/transfer` | Orchestrator | ESL `uuid_transfer <uuid> <target>` |
| E-08 | `POST /api/v1/calls/{id}/record/start` | Orchestrator | ESL `uuid_record <uuid> start <path>` |
| E-09 | `POST /api/v1/calls/{id}/record/stop` | Orchestrator | ESL `uuid_record <uuid> stop` |
| E-10 | `GET /api/v1/calls/{id}/stats` | Orchestrator | ESL `uuid_dump <uuid>` → parse RTP stats |
| E-11 | `POST /api/v1/calls/bridge` | Orchestrator | AI pause → ESL `uuid_bridge` |
| E-12 | `POST /api/v1/calls/unbridge` | Orchestrator | ESL `uuid_transfer park` → AI resume |

### 3.4 IVR State Machine (5 states)

| C++ Pattern | Go Pattern |
|-------------|------------|
| `std::mutex` + deferred callback | Go `chan IvrEvent` + `select` (교착 불가) |
| 5 states: IDLE→MENU→AI_CHAT/TRANSFER/DISCONNECT | 동일 FSM, channel 기반 전이 |
| DTMF routing (1=AI, 0=Transfer, #=Disconnect, *=Menu) | ESL DTMF event → `stateCh` → switch-case |

### 3.5 Operations (8 features)

| # | C++ Feature | To-Be | Implementation |
|---|-------------|-------|----------------|
| O-01 | Recording start/stop | ESL command | `uuid_record <uuid> start/stop <path>` |
| O-02 | Recording cleanup (hourly) | Go goroutine | `time.NewTicker(1*time.Hour)` + `filepath.Walk` |
| O-03 | CDR logging (JSON) | Go goroutine | ESL `CHANNEL_HANGUP_COMPLETE` → slog JSON |
| O-04 | 5-stage graceful shutdown | Go signal handler | SIGINT → HTTP stop → ESL pause → drain → close |
| O-05 | 100+ config (env vars) | Go + FS | `os.Getenv` + FS `vars.xml` |
| O-06 | Production security validation | Go startup check | TLS cert exists, SRTP enabled, API key ≥16 chars |
| O-07 | Rate limiting (token bucket) | Go middleware | `golang.org/x/time/rate` |
| O-08 | Constant-time API key compare | Go middleware | `crypto/subtle.ConstantTimeCompare` |

---

## 4. Data Flow Diagrams

### 4.1 Inbound Call Lifecycle

```
PBX           FreeSWITCH           Orchestrator        Bridge          AI Engine
 │                │                     │                 │               │
 │── INVITE ──→  │                     │                 │               │
 │               │── ESL CHANNEL_CREATE→│                 │               │
 │               │                     │─ TryAcquire()   │               │
 │               │                     │  (atomic CAS)   │               │
 │               │  dialplan:          │                 │               │
 │               │  sleep(200ms)       │                 │               │
 │               │  ring_ready         │                 │               │
 │← 180 ─────── │                     │                 │               │
 │               │  answer             │                 │               │
 │← 200 OK ───  │                     │                 │               │
 │               │  agc                │                 │               │
 │               │  audio_fork ────────│─────────────────│ws://bridge    │
 │               │                     │                 │── gRPC ─────→│
 │               │  start_dtmf         │                 │ StreamSession │
 │               │  park               │                 │               │
 │               │── ESL CHANNEL_PARK──→│                 │               │
 │               │                     │ IVR: IDLE→MENU  │               │
 │               │                     │                 │               │
 │  RTP ←──────→│  FS media thread ───│─────────────────│PCM→VAD→gRPC  │
 │               │                     │                 │               │
 │── BYE ──────→│                     │                 │               │
 │               │── ESL HANGUP ──────→│                 │               │
 │               │                     │ TryRelease()    │               │
 │               │                     │ CDR log         │               │
 │               │                     │                 │── gRPC close →│
```

### 4.2 Audio Pipeline (Steady State)

```
PBX (RTP G.711 8kHz)
    │ 20ms, 160 bytes
    ▼
FreeSWITCH Core
    │ Transcode: G.711→L16, 8kHz→16kHz, AGC
    │ 640 bytes per 20ms frame (320 samples × 2 bytes)
    ▼
mod_audio_fork → WebSocket
    │ L16 PCM, 20ms chunks
    ▼
Bridge: rx goroutine → pcmCh (chan, cap=200)
    │
    ▼
Bridge: vad+grpc goroutine
    │ 512-sample accumulation (32ms @ 16kHz)
    │ Silero VAD inference → is_speaking
    │ AudioChunk { session_id, audio_data, is_speaking }
    │ gRPC stream.Send()
    ▼
AI Engine (gRPC StreamSession)
    │ AiResponse { type, text_content, audio_data, clear_buffer }
    ▼
Bridge: ai-response goroutine
    │ TTS_AUDIO → ttsCh (chan, cap=200)
    │ clear_buffer=true → drain + barge-in
    ▼
Bridge: tx goroutine → WebSocket → FreeSWITCH → PBX (RTP TTS)
```

### 4.3 Barge-in Sequence

```
User speaks (PBX RTP)
    │
    ▼
FS mod_audio_fork → WS Bridge
    │ VAD: is_speaking=true
    │ gRPC: AudioChunk(is_speaking=true)
    ▼
AI Engine
    │ AiResponse(type=END_OF_TURN, clear_buffer=true)
    ▼
Bridge: barge controller
    │ 1. Drain ttsCh (discard all pending TTS frames)
    │ 2. HTTP POST /internal/barge-in/{uuid} → Orchestrator
    ▼
Orchestrator
    │ ESL: uuid_break <fs_uuid> all
    ▼
FreeSWITCH
    │ Current audio playback immediately stopped
    │ Silence RTP packets sent
    ▼
User (PBX): TTS stops immediately, user speech continues
```

### 4.4 Bridge Sequence

```
Client: POST /api/v1/calls/bridge { call_id_1, call_id_2 }
    │
    ▼
Orchestrator
    │ 1. session_a.ai_paused = true
    │ 2. HTTP POST /internal/ai-pause/{uuid_a} → Bridge
    │ 3. ESL: uuid_bridge <fs_uuid_a> <fs_uuid_b>
    ▼
Bridge (session_a): AI gRPC send PAUSED
FreeSWITCH: Direct RTP media between A and B

--- Unbridge ---

Client: POST /api/v1/calls/unbridge { call_id_1, call_id_2 }
    │
    ▼
Orchestrator
    │ 1. ESL: uuid_transfer <uuid_a> park (detach from bridge)
    │ 2. HTTP POST /internal/ai-resume/{uuid_a} → Bridge
    │ 3. session_a.ai_paused = false
    ▼
Bridge (session_a): AI gRPC send RESUMED
```

---

## 5. Migration Plan (4 Phases, 9 Weeks)

### Phase 1: FS + Bridge PoC (Week 1-2)

**Goal**: Validate audio round-trip latency and mod_audio_fork stability

| # | Task | Completion Criteria |
|---|------|---------------------|
| 1-1 | Docker Compose: FS + Bridge + Mock AI | `docker compose up` → SIP INVITE accepted |
| 1-2 | Sofia profile: SRTP, codec order, PBX gateway | Wireshark: SRTP packets verified |
| 1-3 | Dialplan: audio_fork → ws://bridge | Bridge log: WS connection established |
| 1-4 | VAD 32ms inference (same ONNX model) | is_speaking ratio in logs |
| 1-5 | gRPC AudioChunk send/receive | Mock AI received bytes > 0 |
| 1-6 | Barge-in P95 latency | **≤ 100ms (Go/No-Go gate)** |
| 1-7 | E2E audio latency | **P95 ≤ 150ms (Go/No-Go gate)** |

**Go/No-Go**: Call setup success 100%, gRPC connection stability 99.9% (1000 attempts)

### Phase 2: Orchestrator Development (Week 3-6)

**Goal**: Full feature parity — 12 endpoints, IVR, CDR, all operational features

| # | Task | Completion Criteria |
|---|------|---------------------|
| 2-1 | ESL client (event subscription + API) | CHANNEL_CREATE/HANGUP events received |
| 2-2 | 12 HTTP endpoints (100% URL compatible) | curl test all endpoints |
| 2-3 | IVR FSM (channel-based) | TC-05 PASS |
| 2-4 | Session counter (atomic CAS, 100 cap) | 150 concurrent → exactly 100 accepted |
| 2-5 | Rate limiting + ConstantTimeCompare | 429 returned + timing verified |
| 2-6 | CDR logging (JSON) | JSON CDR files generated |
| 2-7 | Recording cleanup goroutine | TC-07 PASS |
| 2-8 | Prometheus /metrics | 20+ metrics exposed |
| 2-9 | 5-stage graceful shutdown | TC-08 PASS |
| 2-10 | Production security validation | TLS/SRTP/API key checks |

**Go/No-Go**: TC-01~TC-08 all PASS, API P95 ≤ 300ms at 50 RPS

### Phase 3: Load Validation + E2E Testing (Week 7-8)

| # | Task | Completion Criteria |
|---|------|---------------------|
| 3-1 | SIPp: 100 concurrent calls, 30min sustained | Audio queue drop ≤ 0.1% |
| 3-2 | Barge-in P95 measurement | ≤ 100ms |
| 3-3 | gRPC reconnection (UNAVAILABLE) | 5× backoff → 100% recovery |
| 3-4 | Memory 72h monitoring | pprof heap: no linear growth |
| 3-5 | Shadow traffic 10% | CDR duration delta ≤ 1s |
| 3-6 | SLO measurement | /health 99.95%, /api 99.9% |

**Go/No-Go**: All SLOs met, no memory leaks

### Phase 4: Blue/Green Cutover (Week 9)

| # | Task | Completion Criteria |
|---|------|---------------------|
| 4-1 | PBX routing 10% → FS | Error rate 0% for 1 hour |
| 4-2 | 25%→50%→100% (24h each) | No SLO violations |
| 4-3 | C++ standby maintained | Rollback procedure documented |
| 4-4 | 100% for 7 days stable | 0 incidents |
| 4-5 | C++ system decommission | Clean shutdown |

---

## 6. Risk Register (Top 10)

| # | Risk | Prob | Impact | Mitigation |
|---|------|------|--------|------------|
| R-01 | mod_audio_fork WS latency > 32ms | Med | High | Phase 1 P99 measurement. If exceeded: loopback WS |
| R-02 | ESL single connection failure | Med | High | Reconnect goroutine + session timeout auto-cleanup |
| R-03 | RTP port exhaustion (100 calls × 2 ports) | Low | High | Reserve 200+ ports (16384-16584) |
| R-04 | Go VAD performance < C++ | Med | Med | Phase 1 benchmark. CGo onnxruntime binding |
| R-05 | gRPC reconnect session ID mismatch | Low | High | New session_id on reconnect + AI reset protocol |
| R-06 | Barge-in HTTP round-trip > 50ms | Med | Med | Loopback deployment. Fallback: Bridge direct ESL |
| R-07 | FS version-specific mod_audio_fork API changes | Low | Med | Pin FS 1.10 LTS + Docker tag lock |
| R-08 | Go goroutine leak | Med | Med | Context propagation + Phase 3 72h pprof |
| R-09 | PBX routing misconfiguration | Low | High | Shadow traffic + admin dry-run before cutover |
| R-10 | SRTP key negotiation failure (FS↔PBX) | Low | High | Start with `optional` → confirm → `mandatory` |

---

## 7. SLO Targets (Preserved from C++)

| Metric | Target | Measurement Point |
|--------|--------|-------------------|
| `/health` uptime | 99.95% (30-day rolling) | Orchestrator HTTP probe |
| `/api/v1/calls` success rate | 99.9% | Orchestrator API |
| Control API P95 latency | ≤ 300ms | Orchestrator HTTP |
| gRPC stream uptime | 99.9% | Bridge metrics |
| Audio queue drop rate | ≤ 0.1% | Bridge `vbgw_grpc_dropped_frames_total` |
| Barge-in P95 latency | ≤ 100ms | Bridge → Orchestrator → ESL |
| gRPC reconnection success | ≥ 99% | Bridge reconnect metrics |

---

## 8. Rollback Plan (RTO: 5 minutes)

### Trigger Conditions (Immediate Rollback)

- `/health` availability < 99.9% (5-minute window)
- Audio queue drop rate > 1%
- gRPC stream failure rate > 0.5%
- P1 incident (call quality complaints)

### Procedure

```
Step 1: PBX routing immediate restore (1-2 min)
  → PBX admin: Route all traffic back to C++ vbgw IP
  → C++ system already running in standby

Step 2: FS system graceful stop
  → Orchestrator: SIGINT → 5-stage shutdown
  → docker compose stop

Step 3: Post-mortem
  → Analyze FS/Orchestrator/Bridge logs + Prometheus metrics
  → Update risk register
  → Retry from Phase 3

Step 4: Re-attempt criteria
  → Root cause fixed and verified
  → Same root cause 2× consecutive → architecture review
```

### Critical Rule

**기존 C++ 프로세스는 Phase 4 완료(100% 전환 후 7일) 시점까지 절대 종료하지 않습니다.**

---

## 9. Deliverables

| Document | Description |
|----------|-------------|
| `freeswitch_migration_v2.md` | This master design document |
| `freeswitch_component_guide.md` | Per-component implementation guide |
| `freeswitch_docker_compose.md` | Docker Compose deployment guide |
| `freeswitch_test_plan.md` | TC-01~TC-08 detailed test procedures |
