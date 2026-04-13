# Voicebot Gateway (VBGW) 프로덕션 개발자 메뉴얼 (Developer Manual)

본 문서는 VBGW 코어 엔진 로직을 수정하거나, PJSIP, gRPC, ONNX Runtime 간의 연동 기능을 덧붙이기 위해 투입되는 **코어 백엔드(C/C++) 개발자**를 위한 매뉴얼입니다.

---

## 1. VBGW 핵심 엔진 아키텍처 및 스레드 모델

### 1.1 스레드 모델 (Thread Safety)
VBGW는 높은 레이턴시 성능을 위해 멀티 스레드 모델을 기반으로 한 복합 엔진입니다.
1. **PJSIP 워커 스레드**: `onCallMediaState`, `onCallState` 등 SIP 규약에 따른 상태 변화 폴링 및 RTP 프레임 인입을 처리.
2. **HTTP 워커 스레드 (Crow)**: API 호출(`/api/v1/calls`)을 처리하며, 내부 컴포넌트(`SessionManager` 등)에 접근.
3. **gRPC 비동기 스트리밍 워커**: AI 서버로 오디오 전송, 및 응답(TTS) 반환 대기 및 큐잉.

> [!WARNING]
> PJSIP의 뮤텍스 락을 쥔 상태에서 외부 개체(`IvrManager` 등)의 콜백을 파이어(Fire)할 경우 교착 상태(Deadlock)에 빠질 수 있습니다. VBGW는 이것을 막기 위해 **copy-and-invoke-outside-lock** 패턴을 엄격하게 준수합니다. 콜백 로직 등 수정 시 절대 `lock_guard` 블록 내에서 외부 콜백을 호출해선 안 됩니다.

### 1.2 생명주기 관리 (Pointer & RAII)
SIP 콜 인프라 특성 상 예기치 못한 BYE나 Cancel에 의해 메모리가 언제든 소등될 수 있습니다.
- `VoicebotCall` 등 생명주기와 엮인 컴포넌트를 비동기로 호출 시, 람다 내부 캡처에서 절대 원시 `[this]`를 캡처하면 안 됩니다.
- 반드시 `weak_from_this()`를 이용하고, 락 상태(`shared_ptr`)를 확보한 후 진행하세요. (Dangling Pointer 에러의 주범입니다.)

---

## 2. 미디어(Audio) 파이프라인 개발

현재 데이터 흐름: **G.711 RTP (Pbx)** → `PJSUA` 해독 → `SpeexDSP` 리샘플(8k to 16k) 및 Denoise → `Silero VAD` 32ms 추론 → `VoicebotAiClient` (gRPC) 전송

### 2.1 VAD(Voice Activity Detection) 튜닝
본 VBGW는 `Silero VAD v5` (ONNX) 모델 전용 래퍼 클래스로 운영됩니다. 
- **위치**: `src/ai/SileroVad.cpp`
- **텐서 사이즈**: 32ms 프레임 단위(16kHz 기준 512 샘플).
- 향후 ONNX 모델 v6 등 모델 업그레이드 시, `state` 텐서의 입출력 차원(Shape) 변화를 주의해야 합니다. (v5는 과거 h/c 분리형 구조에서 단일 state 형인 2x1x128 로 변경된 바 있습니다.)

### 2.2 Bridge 모드 구조
상담원 브릿지 호출(`bridgeWith`) 시 AI를 어떻게 비활성화하는가에 대한 지식입니다.
- **구동 메커니즘**: 브릿지가 맺어지면, `VoicebotCall`은 내부 `VoicebotMediaPort`에 `setAiPaused(true)`를 선언합니다.
- 이에 따라 `onFrameReceived()` 내부에서 AI 엔진을 향하는 청크 복사 체인이 원천 분리(리턴)됩니다. 최적화를 추가하려면 이곳에 로직을 인설트하세요.

---

## 3. 신규 API 및 프로토콜 기능 추가 

기존 코드를 건드려 관리용 HTTP 엔드포인트나 메트릭을 추가하는 가이드입니다.

### 3.1 신규 REST API 작성 기준 (`HttpServer.cpp`)
1. 항상 `X-Admin-Key` 권한 인증 로직(`checkAuth`)을 통과하는 라우터 레벨에서 맵핑하세요.
2. 반사형 XSS 및 인젝션을 막기 위해 외부 주소(`target_uri`) 등의 JSON 필드는 내부 검증 함수를 반드시 거쳐야 합니다.

### 3.2 Protobuf 메시지 업데이트 (`voicebot.proto`)
AI 엔진 단의 전문(`STT/TTS`) 스펙이 변경될 경우:
1. `protos/voicebot.proto` 수정.
2. `CMake` 재빌드 시 `.Codex` 룰에 의해 자동으로 Python 및 C++ 모델 바인딩 파일이 업데이트됩니다.
3. 그 후 `VoicebotAiClient.cpp`의 스트림 읽기(`Read()`) 및 쓰기 구조체 매핑 정보를 동기화해 주시면 됩니다.
