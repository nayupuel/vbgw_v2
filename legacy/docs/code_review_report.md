# 🏗️ VBGW 아키텍처 리뷰 보고서

> **리뷰어**: 콜인프라 시니어 아키텍트  
> **대상 프로젝트**: Voicebot Gateway (VBGW) v1.0.0  
> **리뷰 일시**: 2026-03-29  
> **총평 등급**: ⭐⭐⭐⭐ (4/5) — 탄탄한 기본 설계, 프로덕션 전환을 위한 개선 필요

---

## 📋 총평 (Executive Summary)

VBGW는 PJSIP + gRPC + Silero VAD(ONNX)를 결합한 **C++ 기반 AI 콜봇 음성 게이트웨이**로, 아키텍처 설계가 상당히 잘 정립되어 있습니다. 특히:

**✅ 잘된 점:**
- PJSIP의 미디어 포트 아키텍처를 정확하게 활용 (8kHz↔16kHz 브리지 자동 처리)
- gRPC 양방향 스트리밍 + 지수 백오프 재연결 로직
- Silero VAD Edge 추론 (Pimpl + ONNX Runtime)으로 네트워크 레이턴시 제거
- RAII 기반 자원 관리 (`unique_ptr`, `shared_ptr`)
- `SessionManager` 싱글톤의 TOCTOU 방지 (`tryAddCall`)
- Barge-in 메커니즘 (TTS 버퍼 Flush + VAD 상태 리셋)
- 멀티-스테이지 Docker 빌드 + non-root 실행

**⚠️ 주요 개선 영역:**
- 프로덕션 수준의 SIP/RTP 보안 (SRTP, TLS-SIP) 부재
- Outbound Call 및 IVR 시나리오 미지원
- Health Check / Metrics / Observability 부재
- gRPC 채널 재사용 구조 이슈
- 통화별 리소스 격리 부족

---

## 🔴 CRITICAL — 즉시 수정 필요

### CR-1. gRPC 채널이 통화마다 새로 생성됨 (성능 치명적)

**위치**: [VoicebotCall.cpp:141](file:///Users/kchul199/Desktop/project/antigravity_project/vbgw/src/engine/VoicebotCall.cpp#L141)

```cpp
auto channel = grpc::CreateChannel(ai_addr, creds);
ai_client_ = std::make_shared<VoicebotAiClient>(channel);
```

> [!CAUTION]
> gRPC 채널 생성은 **TCP 연결 수립 + HTTP/2 핸드셰이크 + TLS 협상**을 포함하는 매우 비싼 연산입니다. 100콜 동시 진입 시 100개의 별도 TCP 소켓이 열립니다.

**개선안**: gRPC 채널을 싱글톤 또는 풀링으로 공유
```cpp
// GrpcChannelPool (싱글톤)
class GrpcChannelPool {
public:
    static std::shared_ptr<grpc::Channel> getChannel() {
        static auto channel = grpc::CreateChannel(getAiAddr(), getCreds());
        return channel;
    }
};
```

---

### CR-2. `signal()` 대신 `sigaction()` 사용 필요

**위치**: [main.cpp:33-34](file:///Users/kchul199/Desktop/project/antigravity_project/vbgw/src/main.cpp#L33-L34)

```cpp
signal(SIGINT, signalHandler);
signal(SIGTERM, signalHandler);
```

> [!CAUTION]
> `signal()`은 POSIX에서 동작이 undefined(구현체 의존)이고, 핸들러 실행 중 동일 시그널 수신 시 경쟁 조건 발생. 또한 `std::cout`은 async-signal-safe 함수가 아닙니다.

**개선안**:
```cpp
struct sigaction sa;
sa.sa_handler = signalHandler;
sigemptyset(&sa.sa_mask);
sa.sa_flags = 0;
sigaction(SIGINT, &sa, nullptr);
sigaction(SIGTERM, &sa, nullptr);

// 핸들러 내부에서는 atomic 플래그만 설정
void signalHandler(int) {
    keep_running.store(false, std::memory_order_release);
}
```

---

### CR-3. SIP 트랜스포트에 SRTP / TLS-SIP 지원 없음

**위치**: [VoicebotEndpoint.cpp:47](file:///Users/kchul199/Desktop/project/antigravity_project/vbgw/src/engine/VoicebotEndpoint.cpp#L47)

```cpp
ep->transportCreate(PJSIP_TRANSPORT_UDP, tcfg);
```

> [!CAUTION]
> 현재 SIP 시그널링과 RTP 미디어 모두 **평문 UDP**입니다. 프로덕션 환경에서 통화 내용이 네트워크 상에서 평문으로 노출됩니다.

**개선안**:
- SIP: `PJSIP_TRANSPORT_TLS` 옵션 추가 (SIP over TLS/WSS)
- RTP: `pjmedia_transport_srtp_create()` 로 SRTP 암호화
- 환경변수(예: `SIP_TLS_CERT`, `SRTP_ENABLE`)로 제어

---

### CR-4. 비동기 응답 스레드에서 `thread_desc`이 스택 변수 → Use-After-Free

**위치**: [VoicebotAccount.cpp:85-88](file:///Users/kchul199/Desktop/project/antigravity_project/vbgw/src/engine/VoicebotAccount.cpp#L85-L88)

```cpp
pj_thread_desc thread_desc;       // 스택 로컬!
pj_thread_t* pj_thread = nullptr;
if (!pj_thread_is_registered()) {
    pj_thread_register("vbgw_answer", thread_desc, &pj_thread);
}
```

> [!CAUTION]
> `pj_thread_desc`는 PJLIB가 스레드 생존 기간 동안 내부적으로 참조합니다. 람다 내부 스택 변수는 `std::async` 태스크 완료 시 소멸하지만, PJLIB 스레드 레지스트리는 여전히 이 메모리를 가리킵니다.

**개선안**: `thread_local`로 선언
```cpp
thread_local pj_thread_desc thread_desc;
thread_local pj_thread_t* pj_thread = nullptr;
if (!pj_thread_is_registered()) {
    pj_thread_register("vbgw_answer", thread_desc, &pj_thread);
}
```

---

## 🟠 HIGH — 프로덕션 전 반드시 해결

### H-1. RTP 포트 범위 미설정 → 방화벽 구성 불가

**위치**: [VoicebotEndpoint.cpp](file:///Users/kchul199/Desktop/project/antigravity_project/vbgw/src/engine/VoicebotEndpoint.cpp)

PJSIP의 미디어 포트 범위가 명시적으로 설정되지 않아 OS가 임의 포트를 할당합니다. Dockerfile에서는 `EXPOSE 16000-16100/udp`을 선언했지만, PJSIP 내부에는 이 범위가 전혀 반영되지 않았습니다.

**개선안**:
```cpp
pjsua_media_config media_cfg;
pjsua_media_config_default(&media_cfg);
media_cfg.snd_port_start = 16000;
media_cfg.snd_port_end   = 16100;
```
또는 `EpConfig::mediaConfig`에서 설정:
```cpp
ep_cfg.medConfig.portMin = 16000;
ep_cfg.medConfig.portMax = 16100;
```

---

### H-2. `endSession()` 이후 `stream_->WritesDone()/Finish()` 호출 시 Deadlock 위험

**위치**: [VoicebotAiClient.cpp:160-196](file:///Users/kchul199/Desktop/project/antigravity_project/vbgw/src/ai/VoicebotAiClient.cpp#L160-L196)

```cpp
void VoicebotAiClient::endSession() {
    // ... TryCancel 후 스레드 join ...
    {
        std::lock_guard<std::mutex> lock(stream_mutex_);
        if (stream_) {
            stream_->WritesDone();           // ← Cancelled stream에 WritesDone?
            Status status = stream_->Finish(); // ← 이미 TryCancel된 상태
```

`TryCancel()` 후 `WritesDone()`과 `Finish()`를 호출하는 것은 불필요하며, gRPC 내부적으로 블로킹될 수 있습니다. `TryCancel()` 이후에는 스트림이 이미 CANCELLED 상태이므로.

**개선안**: `context_->TryCancel()` 이후에는 `stream_.reset()`만 호출하고, 정상 종료(non-cancel) 경로에서만 `WritesDone+Finish` 수행.

---

### H-3. Outbound Call(발신) 기능 부재

현재 아키텍처는 **인바운드 콜(수신 전용)** 만 지원합니다. 대부분의 실제 콜봇 시나리오에서는:
- 고객에게 먼저 전화를 거는 **아웃바운드 캠페인**
- **전화 전환(Transfer)**
- **3자 통화(Conference)**

가 필수적입니다.

**제안**: `VoicebotCall::makeCall(const std::string& dest_uri)` 및 REST/gRPC 기반의 호 제어 API 추가

---

### H-4. Health Check가 프로세스 존재 여부만 확인

**위치**: [Dockerfile:72-73](file:///Users/kchul199/Desktop/project/antigravity_project/vbgw/Dockerfile#L72-L73)

```dockerfile
HEALTHCHECK ... CMD pgrep -x vbgw > /dev/null || exit 1
```

`pgrep`은 프로세스가 살아있는지만 확인합니다. SIP 스택 장애, gRPC 연결 불능, 메모리 누수 등 **기능적 정상 동작**은 검증하지 않습니다.

**개선안**: HTTP Health Endpoint 내장
```
GET /health → 200 OK {"sip":"ok","grpc":"ok","active_calls":3,"uptime_secs":1234}
```
Boost.Beast 또는 간단한 TCP 소켓으로 구현 가능.

---

### H-5. Prometheus/OpenTelemetry Metrics 없음

프로덕션 콜센터에서 필수적인 메트릭:
- **동시 호 수** (current/peak)
- **AHT (Average Handling Time)** — 평균 통화 시간
- **ASR (Answer Seizure Ratio)** — 응답률
- **gRPC 레이턴시** (P50/P95/P99)
- **VAD 정확도** (false positive/negative 비율)
- **RTP 패킷 손실률 / Jitter**

**제안**: Prometheus exporter 내장 (HTTP `/metrics` 엔드포인트)

---

### H-6. 환경변수 `getenv()` 호출이 콜 경로(Hot Path)에 산재

**위치**: 
- [VoicebotCall.cpp:60-61](file:///Users/kchul199/Desktop/project/antigravity_project/vbgw/src/engine/VoicebotCall.cpp#L60-L61) — 매 콜마다 `AI_ENGINE_ADDR` 조회
- [VoicebotCall.cpp:96-97](file:///Users/kchul199/Desktop/project/antigravity_project/vbgw/src/engine/VoicebotCall.cpp#L96-L97) — 매 콜마다 `GRPC_USE_TLS` 조회
- [VoicebotAccount.cpp:93](file:///Users/kchul199/Desktop/project/antigravity_project/vbgw/src/engine/VoicebotAccount.cpp#L93) — 매 콜마다 `ANSWER_DELAY_MS` 조회

`getenv()`는 스레드-세이프하지 않은 구현이 존재하며(POSIX에서 thread-safe 보장이 모호), 매 콜 경로에서 반복적으로 호출됩니다.

**개선안**: 시작 시 1회 읽어 구조체에 캐싱하는 `Config` 싱글톤 도입
```cpp
struct AppConfig {
    std::string ai_engine_addr;
    bool grpc_use_tls;
    int answer_delay_ms;
    // ...
    static const AppConfig& instance();
};
```

---

## 🟡 MEDIUM — 안정성/유지보수성 개선

### M-1. `VoicebotCall`의 콜백 Lambda에서 `this` 캡처 → Dangling Pointer 위험

**위치**: [VoicebotCall.cpp:144-167](file:///Users/kchul199/Desktop/project/antigravity_project/vbgw/src/engine/VoicebotCall.cpp#L144-L167)

```cpp
ai_client_->setTtsCallback([this](const uint8_t* data, size_t len) {
    if (media_port_) {
        media_port_->writeTtsAudio(data, len);
    }
});
```

`VoicebotAiClient`는 `shared_ptr`로 관리되지만, 콜백은 `this`(VoicebotCall 원시 포인터)를 캡처합니다. AI 클라이언트의 read 스레드가 아직 실행 중일 때 VoicebotCall이 먼저 소멸하면 dangling 접근이 발생합니다.

**개선안**: `weak_ptr` 또는 `shared_from_this()` 패턴 사용
```cpp
auto weak_self = weak_from_this(); // VoicebotCall이 enable_shared_from_this 상속 필요
ai_client_->setTtsCallback([weak_self](const uint8_t* data, size_t len) {
    if (auto self = weak_self.lock()) {
        if (self->media_port_) {
            self->media_port_->writeTtsAudio(data, len);
        }
    }
});
```

---

### M-2. `RingBuffer` write/read 시 불필요한 mutex 경합

**위치**: [RingBuffer.h](file:///Users/kchul199/Desktop/project/antigravity_project/vbgw/src/utils/RingBuffer.h)

현재 `RingBuffer`는 모든 write/read에 `std::mutex`를 사용합니다. 20ms마다 오디오 프레임이 교차 진입하는 실시간 미디어 경로에서는 lock contention이 발생할 수 있습니다.

둘이 동시 접근이 **단일 Producer(gRPC Rx) - 단일 Consumer(PJSIP Tx)** 패턴이므로:

**개선안**: Lock-free SPSC Ring Buffer 사용
```cpp
// boost::lockfree::spsc_queue 또는 직접 구현
// std::atomic 기반의 head/tail로 lock-free 보장
```

---

### M-3. SIP REGISTER 실패 시 재등록 메커니즘 없음

**위치**: [VoicebotAccount.cpp:28-36](file:///Users/kchul199/Desktop/project/antigravity_project/vbgw/src/engine/VoicebotAccount.cpp#L28-L36)

```cpp
void VoicebotAccount::onRegState(OnRegStateParam& prm) {
    if (ai.regIsActive) { /* 등록됨 */ }
    else { /* 미등록 경고만 출력 */ }
}
```

PBX가 재시작되거나 네트워크가 끊기면 SIP REGISTER가 해제됩니다. 현재는 경고 로그만 출력하고 **자동 재등록을 시도하지 않습니다**.

**개선안**: PJSIP의 `AccountConfig::regConfig.retryIntervalSec`을 설정하거나, `onRegState`에서 수동 재등록 로직 추가.

---

### M-4. 단위 테스트 / 통합 테스트 프레임워크 부재

현재 테스트는 `test.sh`를 통한 수동 E2E 테스트만 존재합니다. C++ 단위 테스트가 전혀 없습니다.

**제안**:
- GoogleTest/GoogleMock 도입
- 핵심 테스트 대상:
  - `RingBuffer`: 경계 조건, wrap-around, 동시 접근
  - `SileroVad`: 고정 PCM 입력에 대한 결정론적 결과 검증
  - `VoicebotAiClient`: Mock gRPC stub을 주입한 워커 스레드 테스트
  - `SessionManager`: 동시 등록/삭제 스트레스 테스트

---

### M-5. `VoicebotEndpoint::shutdown()`에서 `std::cerr` 혼용

**위치**: [VoicebotEndpoint.cpp:64](file:///Users/kchul199/Desktop/project/antigravity_project/vbgw/src/engine/VoicebotEndpoint.cpp#L64)

```cpp
std::cerr << "Shutdown error: " << err.info() << std::endl;
```

나머지 코드는 모두 `spdlog`를 사용하는데, 이 부분만 `std::cerr`입니다. 로그 파일에 기록되지 않습니다.

**개선안**: `spdlog::error("[Endpoint] Shutdown error: {}", err.info());`

---

### M-6. `VoicebotEndpoint`에서 `new` 대신 `make_unique` 사용

**위치**: [VoicebotEndpoint.cpp:12](file:///Users/kchul199/Desktop/project/antigravity_project/vbgw/src/engine/VoicebotEndpoint.cpp#L12)

```cpp
ep.reset(new Endpoint);
```

C++14 이후 Modern C++ 가이드라인에서는 `make_unique`를 권장합니다.

```cpp
ep = std::make_unique<Endpoint>();
```

---

### M-7. TTS 버퍼 크기 하드코딩

**위치**: [VoicebotMediaPort.cpp:15](file:///Users/kchul199/Desktop/project/antigravity_project/vbgw/src/engine/VoicebotMediaPort.cpp#L15)

```cpp
tts_buffer_(std::make_unique<RingBuffer>(160000))  // 16kHz, 16bit, 5초 버퍼
```

160,000 bytes = 16kHz × 2bytes × 5초. 이 값이 하드코딩되어 있어, AI 응답 지연이 5초를 초과하면 오디오가 유실됩니다.

**개선안**: 환경변수 `TTS_BUFFER_SECS`로 조절 가능하게 하고, 기본값은 10초 정도로 여유를 두는 것을 추천합니다.

---

### M-8. `using namespace pj` 남용 — 네임스페이스 오염

**위치**: 여러 `.cpp` 파일

```cpp
using namespace pj;  // VoicebotCall.cpp, VoicebotEndpoint.cpp, VoicebotMediaPort.cpp
```

**개선안**: `.cpp` 파일 내에서라도 필요한 심볼만 명시적으로 사용하거나, 축약 alias 사용.
```cpp
namespace pj = pj;  // already short, but use pj::Call, pj::Account explicitly
```

---

### M-9. Session ID로 integer Call-ID 사용 → UUID 기반 추적 불가

**위치**: [VoicebotCall.cpp:169-171](file:///Users/kchul199/Desktop/project/antigravity_project/vbgw/src/engine/VoicebotCall.cpp#L169-L171)

```cpp
char session_id_str[32];
snprintf(session_id_str, sizeof(session_id_str), "%d", ci.id);
ai_client_->startSession(session_id_str);
```

PJSIP의 `ci.id`는 로컬 정수 인덱스(재사용됨)입니다. 분산 환경에서 로그 추적이 불가능합니다.

**개선안**: `{hostname}:{sip_call_id}:{timestamp}` 또는 UUID v4 기반의 고유 세션 ID 생성.

---

### M-10. `.env` 파일은 `source`로 로드 → 보안 위험

**위치**: [test.sh:54](file:///Users/kchul199/Desktop/project/antigravity_project/vbgw/test.sh#L54), [README.md:103](file:///Users/kchul199/Desktop/project/antigravity_project/vbgw/README.md#L103)

`.env` 파일을 `source`로 로드하는 것은 파일 내 악성 명령어 실행 위험이 있습니다. 또한 `.env`에 비밀번호를 평문 저장하는 것은 보안상 위험합니다.

**개선안**:
- 프로그램이 직접 `.env` 파일을 파싱하는 라이브러리 사용(또는 직접 구현)
- 비밀번호는 HashiCorp Vault, AWS Secrets Manager 등 시크릿 매니저 연동
- `.env` 파일은 `.gitignore`에 포함(이미 되어 있는지 확인 필요)

---

## 🟢 LOW — 코드 품질 / 유지보수성

### L-1. `VoicebotAccount` 소멸자에서 `futures_mutex_` lock 후 `wait()` → Deadlock 가능성

**위치**: [VoicebotAccount.cpp:20-26](file:///Users/kchul199/Desktop/project/antigravity_project/vbgw/src/engine/VoicebotAccount.cpp#L20-L26)

```cpp
~VoicebotAccount() {
    std::lock_guard<std::mutex> lock(futures_mutex_);  
    for (auto& f : answer_futures_) {
        if (f.valid()) f.wait();  // <-- 오래 블로킹될 수 있음
    }
}
```

소멸자에서 mutex를 잡은 채로 `f.wait()` 호출합니다. 만약 해당 future의 태스크가 `futures_mutex_`를 잡으려고 하면 deadlock이 됩니다. 현재 코드에서는 future 내부에서 `futures_mutex_`를 잡지 않으므로 당장은 안전하지만, 향후 리팩토링 시 위험합니다.

**개선안**: 소멸자에서는 futures를 복사(swap)한 뒤 mutex 해제 후 wait.

```cpp
~VoicebotAccount() {
    std::vector<std::future<void>> pending;
    {
        std::lock_guard<std::mutex> lock(futures_mutex_);
        pending.swap(answer_futures_);
    }
    for (auto& f : pending) {
        if (f.valid()) f.wait();
    }
}
```

---

### L-2. `snprintf` 대신 `std::to_string` 사용 가능

**위치**: [VoicebotCall.cpp:169-170](file:///Users/kchul199/Desktop/project/antigravity_project/vbgw/src/engine/VoicebotCall.cpp#L169-L170)

C++20 프로젝트에서 `snprintf`는 불필요합니다.
```cpp
ai_client_->startSession(std::to_string(ci.id));
```

---

### L-3. `VoicebotMediaPort::onFrameRequested`에서 `frame.buf.size() > 0` 검사 위치

**위치**: [VoicebotMediaPort.cpp:68](file:///Users/kchul199/Desktop/project/antigravity_project/vbgw/src/engine/VoicebotMediaPort.cpp#L68)

`frame.buf.size()`가 0인 경우는 PJSIP에서 거의 발생하지 않으며, 발생해도 문제가 없는 코드입니다. 하지만 방어적 코딩 관점에서는 그대로 유지해도 됩니다.

---

### L-4. Proto 파일에 `option` 미설정

**위치**: [voicebot.proto](file:///Users/kchul199/Desktop/project/antigravity_project/vbgw/protos/voicebot.proto)

```protobuf
syntax = "proto3";
package voicebot.ai;
// option go_package, java_package 등 미설정
```

향후 다국어 클라이언트 생성 시 패키지 충돌 방지를 위해 `option` 추가 권장:
```protobuf
option cc_enable_arenas = true;  // C++ 성능 최적화
```

---

### L-5. `compile_commands.json` 파일 내용이 비어있음 (27 bytes)

**위치**: [compile_commands.json](file:///Users/kchul199/Desktop/project/antigravity_project/vbgw/compile_commands.json)

IDE 자동완성/정적 분석을 위해 빌드 후 생성되는 파일이 올바르게 생성되었는지 확인 필요. 보통 `build/compile_commands.json`에서 심링크.

---

### L-6. Emulator에 `__pycache__`과 `.ruff_cache` 커밋

**위치**: [src/emulator/](file:///Users/kchul199/Desktop/project/antigravity_project/vbgw/src/emulator)

`.gitignore`에 포함되어 있는지 확인 필요. Python 바이트코드 캐시는 git에 올릴 필요 없습니다.

---

## 💡 ENHANCEMENT — 차세대 기능 제안

### E-1. CDR (Call Detail Record) 생성

통화 시작/종료 시간, 통화 시간, 종료 사유, VAD 통계, AI 응답 시간 등을 구조화된 형태(JSON)로 기록하여 분석에 활용.

```json
{
    "call_id": "uuid-...",
    "start_time": "2026-03-29T12:00:00Z",
    "end_time": "2026-03-29T12:05:30Z",
    "duration_sec": 330,
    "disconnect_reason": "user_hangup",
    "vad_triggers": 12,
    "bargein_count": 3,
    "grpc_latency_avg_ms": 45
}
```

---

### E-2. 동적 채널 관리 API (REST/gRPC Control Plane)

```
POST   /api/v1/calls          — Outbound Call 발신
DELETE /api/v1/calls/{id}     — 특정 통화 강제 종료
GET    /api/v1/calls          — 현재 활성 통화 목록
PATCH  /api/v1/config         — Runtime 설정 변경 (Hot Reload)
```

---

### E-3. IVR (Interactive Voice Response) 레이어

AI 응답 전 기본 메뉴를 제공하거나, DTMF 입력 처리를 위한 IVR 레이어 추가:
- "1번을 누르시면 상담원 연결, 2번을 누르시면 AI 상담..."
- PJSIP의 `pjmedia_tonegen` / `pjsua_call_send_dtmf` 활용

---

### E-4. AudioCodec 확장 (Opus, G.722)

현재 G.711(PCMA/PCMU) 8kHz만 지원합니다. 고음질 통화를 위해:
- **Opus**: WebRTC 연동 시 필수
- **G.722**: HD Voice (16kHz wideband)
- PJSIP의 코덱 우선순위 설정으로 구현 가능

---

### E-5. Docker Compose / Kubernetes Manifest

```yaml
# docker-compose.yml
services:
  vbgw:
    build: .
    ports:
      - "5060:5060/udp"
      - "16000-16100:16000-16100/udp"
    environment:
      - AI_ENGINE_ADDR=ai-engine:50051
      - GRPC_USE_TLS=1
    depends_on:
      - ai-engine
    deploy:
      replicas: 2
      resources:
        limits:
          cpus: '4'
          memory: '4G'
```

---

### E-6. 녹취(Recording) 기능

규정 준수(컴플라이언스)를 위한 양방향 통화 녹취:
- PJSIP의 `pjsua_recorder_create()` 활용
- 또는 미디어 포트에서 PCM 데이터를 별도 WAV/Opus 파일로 덤프

---

### E-7. Graceful Shutdown 시 Drain 모드

현재 `SIGTERM` 수신 시 즉시 모든 통화를 끊습니다. 프로덕션에서는:
1. 새 호 수신 거부 (503 Service Unavailable)
2. 기존 통화는 자연 종료까지 대기
3. 대기 시간 초과 시 강제 종료

---

### E-8. CMakeLists.txt 크로스 플랫폼 개선

**위치**: [CMakeLists.txt:62,67](file:///Users/kchul199/Desktop/project/antigravity_project/vbgw/CMakeLists.txt#L62)

```cmake
/opt/homebrew/include/onnxruntime  # macOS-only 하드코딩
/opt/homebrew/opt/openssl@3/lib     # macOS-only 하드코딩
```

**개선안**: `find_path` / `find_library`로 플랫폼 독립적 탐색:
```cmake
find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h
    PATHS /opt/homebrew/include/onnxruntime /usr/include/onnxruntime)
```

---

## 📊 파일별 이슈 요약 매트릭스

| 파일 | Critical | High | Medium | Low |
|:---|:---:|:---:|:---:|:---:|
| [main.cpp](file:///Users/kchul199/Desktop/project/antigravity_project/vbgw/src/main.cpp) | CR-2 | — | M-5 | — |
| [VoicebotCall.cpp](file:///Users/kchul199/Desktop/project/antigravity_project/vbgw/src/engine/VoicebotCall.cpp) | CR-1 | H-6 | M-1, M-9 | L-2 |
| [VoicebotAiClient.cpp](file:///Users/kchul199/Desktop/project/antigravity_project/vbgw/src/ai/VoicebotAiClient.cpp) | — | H-2 | — | — |
| [VoicebotEndpoint.cpp](file:///Users/kchul199/Desktop/project/antigravity_project/vbgw/src/engine/VoicebotEndpoint.cpp) | CR-3 | H-1 | M-5, M-6 | — |
| [VoicebotAccount.cpp](file:///Users/kchul199/Desktop/project/antigravity_project/vbgw/src/engine/VoicebotAccount.cpp) | CR-4 | H-6 | M-3 | L-1 |
| [VoicebotMediaPort.cpp](file:///Users/kchul199/Desktop/project/antigravity_project/vbgw/src/engine/VoicebotMediaPort.cpp) | — | — | M-7 | L-3 |
| [RingBuffer.h](file:///Users/kchul199/Desktop/project/antigravity_project/vbgw/src/utils/RingBuffer.h) | — | — | M-2 | — |
| [voicebot.proto](file:///Users/kchul199/Desktop/project/antigravity_project/vbgw/protos/voicebot.proto) | — | — | — | L-4 |
| [CMakeLists.txt](file:///Users/kchul199/Desktop/project/antigravity_project/vbgw/CMakeLists.txt) | — | — | — | E-8 |
| [Dockerfile](file:///Users/kchul199/Desktop/project/antigravity_project/vbgw/Dockerfile) | — | H-4 | — | — |

---

## 🎯 권장 개선 우선순위 (로드맵)

### Phase 1: 즉시 (1~2주)
1. ✅ CR-1: gRPC 채널 싱글톤/풀링
2. ✅ CR-2: `sigaction` 전환
3. ✅ CR-4: `thread_local` pj_thread_desc 수정
4. ✅ H-2: `endSession()` stream cleanup 로직 정리
5. ✅ H-6: `AppConfig` 싱글톤 도입
6. ✅ M-1: 콜백 Lambda dangling pointer 수정

### Phase 2: 단기 (1~2개월)
7. CR-3: SRTP / SIP-TLS 지원
8. H-1: RTP 포트 범위 설정
9. H-4: HTTP Health Endpoint
10. H-5: Prometheus Metrics
11. M-3: SIP 자동 재등록
12. M-4: GoogleTest 도입
13. E-1: CDR 기록

### Phase 3: 중장기 (3~6개월)
14. H-3: Outbound Call 지원
15. E-2: REST/gRPC Control Plane
16. E-3: IVR 레이어
17. E-5: K8s / Docker Compose 배포
18. E-6: 녹취 기능
19. E-7: Graceful Drain 모드

---

> [!TIP]
> 전반적으로 **이 규모의 프로젝트에서 기대할 수 있는 수준 대비 매우 높은 코드 품질**을 보여주고 있습니다. 특히 스레드 안전성에 대한 고려 (C-1~C-6 tag로 표시된 이전 수정사항들), RAII 패턴 적용, 그리고 방어적 코딩(포트 범위 검증, 입력 검증 등)이 인상적입니다. 위의 개선 사항들은 **MVP에서 프로덕션 시스템으로 전환**하기 위한 제안이며, 현재 코드 자체의 기반은 탄탄합니다.
