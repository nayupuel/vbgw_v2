# FreeSWITCH Component Implementation Guide

> **Version**: v1.0.0 | 2026-04-07 | Architect  
> **Parent**: `freeswitch_migration_v2.md`

---

## Table of Contents

1. [FreeSWITCH Configuration](#1-freeswitch-configuration)
2. [Go Orchestrator](#2-go-orchestrator)
3. [WebSocket Bridge Server](#3-websocket-bridge-server)
4. [Inter-Component Communication](#4-inter-component-communication)

---

## 1. FreeSWITCH Configuration

### 1.1 Sofia SIP Profile

**File**: `config/freeswitch/sip_profiles/internal.xml`

```xml
<profile name="internal">
  <!-- SIP Ports -->
  <param name="sip-port" value="$${sip_port}"/>           <!-- default: 5060 -->
  <param name="tls-port" value="$${sip_tls_port}"/>       <!-- default: 5061 -->

  <!-- Security (Production: tls-only=true) -->
  <param name="tls-only" value="$${sip_tls_only}"/>
  <param name="tls-cert-dir" value="$${sip_tls_cert_dir}"/>
  <param name="rtp-secure-media" value="$${srtp_mode}"/>  <!-- optional | mandatory -->

  <!-- NAT Traversal -->
  <param name="ext-rtp-ip" value="$${external_rtp_ip}"/>
  <param name="ext-sip-ip" value="$${external_sip_ip}"/>
  <param name="stun-enabled" value="$${stun_enabled}"/>
  <param name="stun-auto-disable" value="true"/>

  <!-- PRACK & Session Timer -->
  <param name="enable-100rel" value="$${prack_enabled}"/>       <!-- RFC 3262 -->
  <param name="enable-timer" value="$${session_timer_enabled}"/>  <!-- RFC 4028 -->
  <param name="session-timeout" value="$${session_timeout}"/>     <!-- default: 1800 -->
  <param name="minimum-session-expires" value="$${min_se}"/>     <!-- default: 90 -->

  <!-- DTMF -->
  <param name="dtmf-type" value="rfc2833"/>
  <param name="rfc2833-pt" value="101"/>
  <param name="dtmf-duration" value="2000"/>

  <!-- Codec Priority -->
  <param name="inbound-codec-prefs" value="opus@48000h@20i,G722@16000h@20i,PCMU@8000h@20i,PCMA@8000h@20i"/>
  <param name="outbound-codec-prefs" value="opus@48000h@20i,G722@16000h@20i,PCMU@8000h@20i,PCMA@8000h@20i"/>

  <!-- RTP -->
  <param name="rtp-start-port" value="$${rtp_start_port}"/>  <!-- default: 16384 -->
  <param name="rtp-end-port" value="$${rtp_end_port}"/>      <!-- default: 16584 -->

  <!-- Jitter Buffer -->
  <param name="jitter-buffer-ms" value="$${jb_init}:$${jb_min}:$${jb_max}"/>

  <!-- REFER/REPLACES -->
  <param name="accept-blind-transfer" value="true"/>

  <!-- PBX Registration Gateway -->
  <gateways>
    <gateway name="pbx-main">
      <param name="username" value="$${pbx_username}"/>
      <param name="password" value="$${pbx_password}"/>
      <param name="proxy" value="$${pbx_host}"/>
      <param name="register" value="$${pbx_register}"/>       <!-- true | false -->
      <param name="retry-seconds" value="60"/>
      <param name="expire-seconds" value="3600"/>
    </gateway>
  </gateways>
</profile>
```

### 1.2 Dialplan (Inbound Call Handling)

**File**: `config/freeswitch/dialplan/default.xml`

```xml
<include>
  <context name="default">

    <!-- Voicebot Inbound Handler -->
    <extension name="voicebot-inbound">
      <condition field="destination_number" expression="^(.+)$">
        <!-- Step 1: Answer delay (configurable, default 200ms) -->
        <action application="sleep" data="$${answer_delay_ms}"/>

        <!-- Step 2: 180 Ringing -->
        <action application="ring_ready"/>

        <!-- Step 3: 200 OK -->
        <action application="answer"/>

        <!-- Step 4: Audio processing — AGC -->
        <action application="agc" data="1"/>

        <!-- Step 5: Start audio streaming to WebSocket Bridge -->
        <!-- mod_audio_fork: L16 PCM 16kHz mono, 20ms frames -->
        <action application="audio_fork"
                data="ws://$${bridge_host}:$${bridge_ws_port}/audio/${uuid} 16000 mono"/>

        <!-- Step 6: Enable DTMF detection -->
        <action application="start_dtmf"/>

        <!-- Step 7: Park — Orchestrator takes control via ESL -->
        <action application="park"/>
      </condition>
    </extension>

  </context>
</include>
```

### 1.3 Event Socket Configuration

**File**: `config/freeswitch/autoload_configs/event_socket.conf.xml`

```xml
<configuration name="event_socket.conf" description="Event Socket">
  <settings>
    <param name="listen-ip" value="0.0.0.0"/>
    <param name="listen-port" value="8021"/>
    <param name="password" value="$${esl_password}"/>
    <param name="apply-inbound-acl" value="loopback.auto"/>
  </settings>
</configuration>
```

### 1.4 Variables (Environment Mapping)

**File**: `config/freeswitch/vars.xml`

```xml
<include>
  <!-- SIP -->
  <X-PRE-PROCESS cmd="set" data="sip_port=${SIP_PORT:-5060}"/>
  <X-PRE-PROCESS cmd="set" data="sip_tls_port=${SIP_TLS_PORT:-5061}"/>
  <X-PRE-PROCESS cmd="set" data="sip_tls_only=${SIP_TLS_ONLY:-false}"/>
  <X-PRE-PROCESS cmd="set" data="sip_tls_cert_dir=${SIP_TLS_CERT_DIR:-/etc/freeswitch/tls}"/>
  <X-PRE-PROCESS cmd="set" data="srtp_mode=${SRTP_MODE:-optional}"/>

  <!-- NAT -->
  <X-PRE-PROCESS cmd="set" data="external_rtp_ip=${EXTERNAL_RTP_IP:-auto-nat}"/>
  <X-PRE-PROCESS cmd="set" data="external_sip_ip=${EXTERNAL_SIP_IP:-auto-nat}"/>
  <X-PRE-PROCESS cmd="set" data="stun_enabled=${STUN_ENABLED:-true}"/>

  <!-- Session -->
  <X-PRE-PROCESS cmd="set" data="prack_enabled=${PRACK_ENABLED:-true}"/>
  <X-PRE-PROCESS cmd="set" data="session_timer_enabled=${SESSION_TIMER_ENABLED:-true}"/>
  <X-PRE-PROCESS cmd="set" data="session_timeout=${SESSION_TIMEOUT:-1800}"/>
  <X-PRE-PROCESS cmd="set" data="min_se=${MIN_SE:-90}"/>
  <X-PRE-PROCESS cmd="set" data="answer_delay_ms=${ANSWER_DELAY_MS:-200}"/>

  <!-- RTP -->
  <X-PRE-PROCESS cmd="set" data="rtp_start_port=${RTP_PORT_MIN:-16384}"/>
  <X-PRE-PROCESS cmd="set" data="rtp_end_port=${RTP_PORT_MAX:-16584}"/>

  <!-- Jitter Buffer -->
  <X-PRE-PROCESS cmd="set" data="jb_init=${JB_INIT_MS:-100}"/>
  <X-PRE-PROCESS cmd="set" data="jb_min=${JB_MIN_MS:-60}"/>
  <X-PRE-PROCESS cmd="set" data="jb_max=${JB_MAX_MS:-500}"/>

  <!-- PBX -->
  <X-PRE-PROCESS cmd="set" data="pbx_host=${PBX_HOST:-}"/>
  <X-PRE-PROCESS cmd="set" data="pbx_username=${PBX_USERNAME:-}"/>
  <X-PRE-PROCESS cmd="set" data="pbx_password=${PBX_PASSWORD:-}"/>
  <X-PRE-PROCESS cmd="set" data="pbx_register=${PBX_REGISTER:-false}"/>

  <!-- Bridge -->
  <X-PRE-PROCESS cmd="set" data="bridge_host=${BRIDGE_HOST:-bridge}"/>
  <X-PRE-PROCESS cmd="set" data="bridge_ws_port=${BRIDGE_WS_PORT:-8090}"/>

  <!-- ESL -->
  <X-PRE-PROCESS cmd="set" data="esl_password=${ESL_PASSWORD:-ClueCon}"/>
</include>
```

---

## 2. Go Orchestrator

### 2.1 Package Structure

```
orchestrator/
├── cmd/
│   └── main.go                     # Entry point: config → DI → server start
├── internal/
│   ├── config/
│   │   └── config.go               # 100+ env vars (viper or envconfig)
│   ├── esl/
│   │   ├── client.go               # ESL TCP connection, event loop, API dispatch
│   │   ├── event.go                # ESL event parsing (key=value → struct)
│   │   └── commands.go             # uuid_* wrapper functions
│   ├── session/
│   │   ├── manager.go              # sync.Map + atomic.Int64 (TOCTOU-safe)
│   │   └── model.go                # SessionState struct
│   ├── ivr/
│   │   └── machine.go              # Channel-based FSM (no mutex, no deadlock)
│   ├── api/
│   │   ├── server.go               # HTTP server (net/http + chi router)
│   │   ├── middleware.go            # Auth (ConstantTimeCompare) + Rate limit
│   │   ├── calls.go                # POST /api/v1/calls
│   │   ├── control.go              # DTMF, transfer, record, bridge endpoints
│   │   ├── stats.go                # GET /api/v1/calls/{id}/stats
│   │   └── health.go               # /live, /ready, /health, /metrics
│   ├── cdr/
│   │   └── logger.go               # CHANNEL_HANGUP_COMPLETE → JSON CDR
│   ├── recording/
│   │   └── cleaner.go              # Hourly ticker: age (30d) + quota (1GB)
│   └── metrics/
│       └── prometheus.go            # 20+ gauges, counters, histograms
├── go.mod
└── go.sum
```

### 2.2 Key Go Dependencies

```
module vbgw-orchestrator

go 1.22

require (
    github.com/go-chi/chi/v5         // HTTP router
    github.com/prometheus/client_golang // Prometheus metrics
    github.com/google/uuid            // UUID v4
    golang.org/x/time                 // Rate limiter (token bucket)
    golang.org/x/crypto               // subtle.ConstantTimeCompare
)
```

### 2.3 ESL Client Design

**Connection & Event Loop**

```
ESL TCP connection to FreeSWITCH:8021
  → auth: "auth <password>\n\n"
  → subscribe: "event plain CHANNEL_CREATE CHANNEL_ANSWER CHANNEL_HANGUP_COMPLETE
                            CHANNEL_PARK DTMF CHANNEL_BRIDGE CHANNEL_UNBRIDGE
                            CUSTOM sofia::register sofia::unregister\n\n"
  → reader goroutine: line-by-line parsing → Event struct → dispatcher channel
  → dispatcher goroutine: fan-out to per-session channels
```

**ESL Command Wrappers**

```go
// commands.go — Each wrapper sends ESL API command and returns response

func (c *Client) Originate(target, calleeId string) (string, error)
    // "bgapi originate {origination_uuid=<uuid>}sofia/gateway/pbx-main/<target> &park()"

func (c *Client) SendDtmf(uuid, digits string) error
    // "api uuid_send_dtmf <uuid> <digits>"

func (c *Client) Transfer(uuid, target string) error
    // "api uuid_transfer <uuid> <target> XML default"

func (c *Client) Bridge(uuidA, uuidB string) error
    // "api uuid_bridge <uuidA> <uuidB>"

func (c *Client) Unbridge(uuid string) error
    // "api uuid_transfer <uuid> -both park"

func (c *Client) RecordStart(uuid, path string) error
    // "api uuid_record <uuid> start <path>"

func (c *Client) RecordStop(uuid string) error
    // "api uuid_record <uuid> stop"

func (c *Client) Break(uuid string) error
    // "api uuid_break <uuid> all"

func (c *Client) Kill(uuid string) error
    // "api uuid_kill <uuid>"

func (c *Client) Dump(uuid string) (map[string]string, error)
    // "api uuid_dump <uuid>" → parse key=value pairs

func (c *Client) Pause() error
    // "api fsctl pause"

func (c *Client) Hupall() error
    // "api hupall"
```

### 2.4 Session Manager (TOCTOU-Safe)

```go
// manager.go

type Manager struct {
    sessions sync.Map              // map[string]*SessionState (key: session_id)
    count    atomic.Int64          // atomic counter for capacity check
    maxCalls int64                 // from config (default 100)
}

// TryAcquire — Atomically check + increment (no TOCTOU race)
func (m *Manager) TryAcquire() bool {
    for {
        cur := m.count.Load()
        if cur >= m.maxCalls {
            return false
        }
        if m.count.CompareAndSwap(cur, cur+1) {
            return true
        }
        // CAS failed — another goroutine incremented concurrently, retry
    }
}

// Release — Decrement counter + remove session
func (m *Manager) Release(sessionID string) {
    if _, loaded := m.sessions.LoadAndDelete(sessionID); loaded {
        m.count.Add(-1)
    }
}
```

### 2.5 IVR State Machine (Channel-Based, Deadlock-Free)

```go
// machine.go

type State int
const (
    Idle State = iota
    Menu
    AiChat
    Transfer
    Disconnect
)

type EventType int
const (
    DtmfEvent EventType = iota
    HangupEvent
    ActivateMenuEvent
)

type IvrEvent struct {
    Type  EventType
    Digit string  // for DtmfEvent: "0"-"9", "*", "#"
}

type Machine struct {
    state   State
    eventCh chan IvrEvent  // buffered (16)
    // Callbacks (registered at creation)
    onForwardDtmf  func(digit string)
    onTransfer     func()
    onDisconnect   func()
    onRepeatMenu   func()
    onEnterAiChat  func()
}

// Run — Single goroutine, select-based (no mutex needed)
func (m *Machine) Run(ctx context.Context) {
    for {
        select {
        case <-ctx.Done():
            return
        case evt := <-m.eventCh:
            m.handleEvent(evt)
        }
    }
}

// Transition table
func (m *Machine) handleEvent(evt IvrEvent) {
    switch evt.Type {
    case ActivateMenuEvent:
        m.state = Menu
        m.onRepeatMenu()

    case DtmfEvent:
        switch m.state {
        case Menu:
            switch evt.Digit {
            case "1": m.state = AiChat; m.onEnterAiChat()
            case "0": m.state = Transfer; m.onTransfer()
            case "#": m.state = Disconnect; m.onDisconnect()
            case "*": m.onRepeatMenu()
            default:  m.onForwardDtmf(evt.Digit)
            }
        case AiChat:
            switch evt.Digit {
            case "0": m.state = Transfer; m.onTransfer()
            case "*": m.state = Menu; m.onRepeatMenu()
            case "#": m.state = Disconnect; m.onDisconnect()
            default:  m.onForwardDtmf(evt.Digit)
            }
        }

    case HangupEvent:
        m.state = Idle
    }
}
```

### 2.6 Graceful Shutdown (5-Stage)

```go
// main.go

func main() {
    ctx, cancel := context.WithCancel(context.Background())
    sigCh := make(chan os.Signal, 1)
    signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)

    go func() {
        <-sigCh
        slog.Info("[Shutdown 1/5] HTTP server: rejecting new requests")
        httpServer.SetKeepAlivesEnabled(false)
        httpServer.Shutdown(ctx)

        slog.Info("[Shutdown 2/5] ESL: fsctl pause — no new calls")
        eslClient.Pause()

        slog.Info("[Shutdown 3/5] Draining active sessions", "count", sessionMgr.Count())
        drainCtx, drainCancel := context.WithTimeout(ctx, 30*time.Second)
        defer drainCancel()
        sessionMgr.WaitAllDrained(drainCtx)

        slog.Info("[Shutdown 4/5] Closing Bridge gRPC streams")
        // Bridge sessions are closed via session cleanup

        slog.Info("[Shutdown 5/5] ESL connection closed")
        eslClient.Close()
        cancel()
    }()

    // ... server start ...
    <-ctx.Done()
    os.Exit(0)
}
```

### 2.7 HTTP API Middleware

**Rate Limiting (Token Bucket)**

```go
// middleware.go

func RateLimitMiddleware(rps float64, burst int) func(http.Handler) http.Handler {
    limiter := rate.NewLimiter(rate.Limit(rps), burst)
    return func(next http.Handler) http.Handler {
        return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
            if !limiter.Allow() {
                w.Header().Set("Retry-After", "1")
                http.Error(w, "Too Many Requests", http.StatusTooManyRequests)
                return
            }
            next.ServeHTTP(w, r)
        })
    }
}
```

**Constant-Time API Key Auth**

```go
func AuthMiddleware(expectedKey string) func(http.Handler) http.Handler {
    expected := []byte(expectedKey)
    return func(next http.Handler) http.Handler {
        return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
            provided := []byte(r.Header.Get("X-Admin-Key"))
            if subtle.ConstantTimeCompare(expected, provided) != 1 {
                http.Error(w, "Forbidden", http.StatusForbidden)
                return
            }
            next.ServeHTTP(w, r)
        })
    }
}
```

### 2.8 Prometheus Metrics

```go
// prometheus.go — Mirror existing C++ RuntimeMetrics

var (
    ActiveCalls = promauto.NewGauge(prometheus.GaugeOpts{
        Name: "vbgw_active_calls",
    })
    SipRegistered = promauto.NewGauge(prometheus.GaugeOpts{
        Name: "vbgw_sip_registered",
    })
    GrpcActiveSessions = promauto.NewGauge(prometheus.GaugeOpts{
        Name: "vbgw_grpc_active_sessions",
    })
    GrpcDroppedFrames = promauto.NewCounter(prometheus.CounterOpts{
        Name: "vbgw_grpc_dropped_frames_total",
    })
    GrpcStreamErrors = promauto.NewCounter(prometheus.CounterOpts{
        Name: "vbgw_grpc_stream_errors_total",
    })
    GrpcReconnectAttempts = promauto.NewCounter(prometheus.CounterOpts{
        Name: "vbgw_grpc_reconnect_attempts_total",
    })
    VadSpeechEvents = promauto.NewCounter(prometheus.CounterOpts{
        Name: "vbgw_vad_speech_events_total",
    })
    BargeInEvents = promauto.NewCounter(prometheus.CounterOpts{
        Name: "vbgw_bargein_events_total",
    })
    ApiOutboundRequests = promauto.NewCounter(prometheus.CounterOpts{
        Name: "vbgw_admin_api_outbound_requests_total",
    })
    ApiOutboundRejectedRateLimited = promauto.NewCounter(prometheus.CounterOpts{
        Name: "vbgw_admin_api_outbound_rejected_rate_limited_total",
    })
    // ... 10+ more counters matching C++ RuntimeMetrics
)
```

---

## 3. WebSocket Bridge Server

### 3.1 Package Structure

```
bridge/
├── cmd/
│   └── main.go                     # Entry point
├── internal/
│   ├── config/
│   │   └── config.go               # AI_GRPC_ADDR, ONNX_MODEL_PATH, etc.
│   ├── ws/
│   │   ├── server.go               # HTTP server + WS upgrade + per-UUID routing
│   │   └── session.go              # Per-session 4-goroutine orchestrator
│   ├── vad/
│   │   └── silero.go               # onnxruntime-go + silero_vad.onnx
│   ├── grpc/
│   │   ├── client.go               # voicebot.proto gRPC bidirectional client
│   │   └── retry.go                # Exponential backoff (5 retries, 4s max)
│   ├── tts/
│   │   └── buffer.go               # Buffered channel (cap=200), oldest-drop
│   └── barge/
│       └── controller.go           # clear_buffer → HTTP POST → Orchestrator
├── go.mod
└── go.sum
```

### 3.2 Per-Session Goroutine Model

```
WebSocket Accept (URL: /audio/{uuid})
    │
    ├── rx goroutine ─────────────────────────────────────┐
    │   WS ReadMessage → pcmCh (chan []byte, cap=200)      │
    │   Overflow: drop oldest frame, increment metric      │
    │                                                       │
    ├── vad+grpc goroutine ───────────────────────────────┐
    │   pcmCh → 512-sample accumulator (32ms @ 16kHz)      │
    │   Silero VAD inference → is_speaking                  │
    │   AudioChunk { session_id, audio_data, is_speaking }  │
    │   gRPC stream.Send()                                  │
    │                                                       │
    ├── ai-response goroutine ────────────────────────────┐
    │   gRPC stream.Recv()                                  │
    │   TTS_AUDIO → ttsCh (chan []byte, cap=200)            │
    │   STT_RESULT → log (REDACTED for PII)                 │
    │   clear_buffer=true → drain ttsCh + barge-in HTTP     │
    │   END_OF_TURN → CDR metric increment                  │
    │                                                       │
    └── tx goroutine ─────────────────────────────────────┐
        ttsCh → WS WriteMessage (L16 PCM → FS playback)    │
        If ai_paused: skip (don't send to FS)               │
```

### 3.3 VAD Implementation (32ms Window)

```go
// silero.go

type SileroVad struct {
    session *ort.Session
    buffer  []int16  // Rolling PCM buffer
    mu      sync.Mutex
}

const vadWindowSamples = 512  // 32ms @ 16kHz

// Process — Accumulate 20ms frames, run inference at 32ms
func (v *SileroVad) Process(pcm16 []int16) (isSpeaking bool) {
    v.mu.Lock()
    defer v.mu.Unlock()

    v.buffer = append(v.buffer, pcm16...)

    for len(v.buffer) >= vadWindowSamples {
        window := v.buffer[:vadWindowSamples]
        v.buffer = v.buffer[vadWindowSamples:]

        // ONNX inference: same model as C++ (models/silero_vad.onnx)
        prob := v.infer(window)  // returns float32 [0.0, 1.0]
        isSpeaking = prob > 0.5
    }
    return
}
```

### 3.4 gRPC Client with Retry

```go
// retry.go — Mirror C++ VoicebotAiClient reconnection policy

const (
    maxRetries    = 5
    maxBackoffMs  = 4000
    initialBackoff = 100 * time.Millisecond
)

func (c *Client) reconnectLoop(ctx context.Context) {
    backoff := initialBackoff
    for attempt := 0; attempt < maxRetries; attempt++ {
        select {
        case <-ctx.Done():
            return
        case <-time.After(backoff):
        }

        err := c.connect(ctx)
        if err == nil {
            return  // Success
        }

        // Permanent failure detection (same as C++)
        code := status.Code(err)
        if isPermanent(code) {
            // INVALID_ARGUMENT, PERMISSION_DENIED, UNAUTHENTICATED,
            // FAILED_PRECONDITION, UNIMPLEMENTED, INTERNAL, DATA_LOSS
            c.onError(err)
            return
        }

        // Exponential backoff with jitter
        backoff = min(backoff*2, maxBackoffMs)
        jitter := time.Duration(rand.Int63n(int64(backoff) / 4))
        backoff += jitter
    }
    c.onError(fmt.Errorf("max retries (%d) exceeded", maxRetries))
}
```

### 3.5 Barge-in Controller

```go
// controller.go

type BargeController struct {
    orchestratorURL string  // e.g., "http://orchestrator:8080"
    httpClient      *http.Client
}

// HandleClearBuffer — Called when AI sends clear_buffer=true
func (b *BargeController) HandleClearBuffer(ctx context.Context, uuid string, ttsCh chan []byte) {
    // Step 1: Drain TTS channel (non-blocking)
    drained := 0
    for {
        select {
        case <-ttsCh:
            drained++
        default:
            goto done
        }
    }
done:
    slog.Info("TTS buffer drained", "uuid", uuid, "frames_discarded", drained)

    // Step 2: Notify Orchestrator → ESL uuid_break
    url := fmt.Sprintf("%s/internal/barge-in/%s", b.orchestratorURL, uuid)
    req, _ := http.NewRequestWithContext(ctx, "POST", url, nil)
    resp, err := b.httpClient.Do(req)
    if err != nil {
        slog.Error("barge-in notification failed", "uuid", uuid, "err", err)
        return
    }
    defer resp.Body.Close()
    slog.Info("barge-in executed", "uuid", uuid, "status", resp.StatusCode)
}
```

### 3.6 Internal HTTP API (Bridge ← Orchestrator)

| Endpoint | Direction | Purpose |
|----------|-----------|---------|
| `POST /internal/ai-pause/{uuid}` | Orchestrator → Bridge | Pause gRPC send (bridge mode) |
| `POST /internal/ai-resume/{uuid}` | Orchestrator → Bridge | Resume gRPC send |
| `POST /internal/dtmf/{uuid}` | Orchestrator → Bridge | Forward DTMF to AI via gRPC |
| `GET /internal/health` | Orchestrator → Bridge | Bridge process health |

---

## 4. Inter-Component Communication

### 4.1 Network Topology

```
┌──────────────────────────────────────────────┐
│ Docker Network: vbgw-net (bridge mode)        │
│                                                │
│  freeswitch:5060  ←→  PBX (external)          │
│  freeswitch:8021  ←→  orchestrator (internal)  │
│  bridge:8090      ←→  freeswitch (WS)          │
│  bridge:8091      ←→  orchestrator (HTTP)       │
│  orchestrator:8080 ←→ external clients          │
│  ai-engine:50051  ←→  bridge (gRPC)            │
└──────────────────────────────────────────────┘
```

### 4.2 Event Flow (ESL → Orchestrator)

```
ESL Event                    Orchestrator Handler              Action
────────────────────────────────────────────────────────────────────────
CHANNEL_CREATE              onChannelCreate()                 TryAcquire + create session
CHANNEL_ANSWER              onChannelAnswer()                 Start IVR (IDLE→MENU)
CHANNEL_PARK                onChannelPark()                   Ready for ESL control
DTMF                        onDtmf()                          IVR machine.eventCh ← DtmfEvent
CHANNEL_BRIDGE              onChannelBridge()                 Log bridge event
CHANNEL_UNBRIDGE            onChannelUnbridge()               Log unbridge event
CHANNEL_HANGUP_COMPLETE     onChannelHangup()                 CDR log + Release session
CUSTOM sofia::register      onSofiaRegister()                 Update SIP registered metric
CUSTOM sofia::unregister    onSofiaUnregister()               Update SIP registered metric
```

### 4.3 Configuration Mapping (C++ → Go + FS)

| C++ Env Var | Go/FS Equivalent | Location |
|-------------|-----------------|----------|
| `SIP_PORT` | `$${sip_port}` | FS vars.xml |
| `SIP_USE_TLS` | `$${sip_tls_only}` | FS vars.xml |
| `SRTP_ENABLE` + `SRTP_MANDATORY` | `$${srtp_mode}` | FS vars.xml |
| `PBX_URI` | `$${pbx_host}` | FS vars.xml |
| `PBX_USERNAME` / `PBX_PASSWORD` | `$${pbx_username}` / `$${pbx_password}` | FS vars.xml |
| `AI_ENGINE_ADDR` | `AI_GRPC_ADDR` | Bridge env |
| `GRPC_USE_TLS` | `AI_GRPC_TLS` | Bridge env |
| `SILERO_VAD_MODEL_PATH` | `ONNX_MODEL_PATH` | Bridge env |
| `MAX_CONCURRENT_CALLS` | `MAX_SESSIONS` | Orchestrator env |
| `ANSWER_DELAY_MS` | `$${answer_delay_ms}` | FS vars.xml |
| `HTTP_PORT` | `HTTP_PORT` | Orchestrator env |
| `ADMIN_API_KEY` | `ADMIN_API_KEY` | Orchestrator env |
| `ADMIN_API_RATE_LIMIT_RPS` | `RATE_LIMIT_RPS` | Orchestrator env |
| `LOG_LEVEL` | `LOG_LEVEL` | Orchestrator + Bridge env |
| `CALL_RECORDING_ENABLE` | `RECORDING_ENABLE` | Orchestrator env |
| `CALL_RECORDING_DIR` | `RECORDING_DIR` | Orchestrator env (FS 마운트) |
| `CALL_RECORDING_MAX_DAYS` | `RECORDING_MAX_DAYS` | Orchestrator env |
| `CALL_RECORDING_MAX_MB` | `RECORDING_MAX_MB` | Orchestrator env |
| `VBGW_PROFILE` | `RUNTIME_PROFILE` | Orchestrator env |
