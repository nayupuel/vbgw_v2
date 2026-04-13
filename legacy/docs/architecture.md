# AI 콜봇(Voicebot) 인프라 연동 솔루션 아키텍처 설계서

## 1. 개요 (Overview)
본 문서는 고객의 전화를 받아(PBX/SBC 연동) 음성을 인식(STT)하고, 의도를 파악(NLU)하여, 적절한 답변을 음성으로 송출(TTS)하는 **AI 콜봇 서비스의 핵심 '통화 제어 및 미디어 게이트웨이(Voice Gateway)' 솔루션** 설계서입니다. 

순수 시그널링(SIP) 제어뿐만 아니라, **실시간 RTP(음성 패킷) 스트리밍 처리가 아키텍처의 가장 중요한 핵심**이 됩니다.

## 2. 최적화 도서관(Library) 재검토 및 최종 선정 (Technology Stack)

콜봇 게이트웨이는 일반 PBX와 달리 **"수천 건의 SIP 시그널링 유지 + 실시간 양방향 RTP 미디어 트랜스코딩 + 방대한 AI 오디오 스트리밍"**이라는 극한의 I/O 부하를 견뎌야 합니다. 이를 위해 엄격히 검토하여 선정한 **최상위 최적화 C/C++ 라이브러리 스택**은 다음과 같습니다.

### 2.1 SIP & Media Core: PJSIP (C로 작성, C++ 지원) 🏆
*   **선정 이유:** 트래픽 라우팅에 특화된 Kamailio나, 무거운 다목적 스위치인 FreeSWITCH에 비해 **"직접 만든 C++ 서버 안에서 오디오 프레임(PCM 버퍼)을 마이크로초 단위로 주입/추출하기 가장 가볍고 정교한 API"**를 제공합니다. 
*   **특장점:** VAD(Voice Activity Detection), Adaptive Jitter Buffer, G.711(PCMA/PCMU) 인코딩/디코딩 모듈이 C 레벨로 강력하게 최적화되어 내장되어 있으므로 콜봇용 미디어 핸들링의 표준입니다.

### 2.2 비동기 I/O 및 멀티스레드 제어: Boost.Asio (C++17/20) 🏆
*   **선정 이유:** 콜봇은 네트워킹의 집합체입니다. 수많은 소켓 연결(내부 API용 TCP/WS, 기타 통신)과 워커 스레드 관리, 타이머(타임아웃 처리)를 블로킹 락(Lock) 없이 구현해야 합니다. 리눅스의 `epoll`과 Mac의 `kqueue`를 추상화한 **Boost.Asio**는 스레드 컨텍스트 스위칭 지연을 가장 완벽하게 제거해주는 C++ 비동기 네트워킹의 De facto(사실상 표준)입니다.

### 2.3 AI 오디오 스트리밍 프로토콜: gRPC (C++ & Protocol Buffers) 🏆
*   **선정 이유:** 양방향으로 20ms 간격으로 잘게 쪼개진 오디오 바이너리를 STT/TTS 엔진으로 전달해야 합니다. REST API나 일반 WebSocket을 썼을 때 발생하는 HTTP 헤더 오버헤드를, HTTP/2 기반의 **gRPC Bi-directional Streaming**이 드라마틱하게 줄여줍니다. 처리량 면에서 경쟁 기술보다 최고 10배 빠릅니다.

### 2.4 오디오 리샘플링: PJSIP 미디어 브리지 (내장) + SpeexDSP (예비) 🏆
*   **현재 구현:** PBX에서 수신되는 G.711 8kHz RTP와 내부 미디어포트(16kHz) 사이의 리샘플링은 **PJSIP 내장 미디어 브리지**가 자동으로 처리합니다. `VoicebotMediaPort`를 16kHz 포맷으로 생성하면 PJSIP이 `pjmedia_resample`을 사용하여 8kHz↔16kHz 변환을 투명하게 수행합니다.
*   **SpeexDSP 역할:** 빌드 의존성으로 포함되어 있으며, 추후 고정밀 리샘플링·에코 캔슬링·노이즈 감소 기능 추가 시 직접 활용 예정입니다. 현재 단계에서는 PJSIP 브리지를 통해 간접적으로 사용됩니다.

### 2.5 빌드 환경 및 스크립팅: CMake + Ninja
*   **선정 이유:** PJSIP, gRPC, Boost와 같은 초대형 라이브러리의 의존성을 맥(Mac) 개발 환경과 리눅스 배포 환경에서 동일하게 유지/빌드할 수 있는 유일한 시스템입니다. 빌드 생성기는 5배 빠른 Ninja를 도입해 개발 생산성을 올립니다.

## 3. 콜봇 특화 시스템 아키텍처 (System Architecture)

```mermaid
graph TD
    %% 사용자 및 전화망
    User[고객 📱] <--> |Voice| PBX[PBX / SBC]
    
    %% 콜봇 게이트웨이 (이번에 개발할 C++ 솔루션)
    subgraph Callbot Gateway [Callbot Infrastructure Solution (C++)]
        SIP_End[SIP Endpoint]
        Media_Port[RTP Media Port / Jitter Buffer]
        Orchestrator[Dialog & Session Orchestrator]
        AI_Connector[AI Stream Connector (gRPC/WS)]
        
        SIP_End <--> |제어| Orchestrator
        Orchestrator <--> |제어| Media_Port
        Media_Port <--> |PCM Audio| AI_Connector
    end
    
    PBX <--> |SIP| SIP_End
    PBX <--> |RTP (G.711)| Media_Port
    
    %% AI 백엔드 엔진
    subgraph AI Engines
        STT[STT Engine (음성인식)]
        NLU[NLU Engine (자연어이해)]
        TTS[TTS Engine (음성합성)]
        
        STT --> |Text| NLU
        NLU --> |Text| TTS
    end
    
    AI_Connector --> |1. Audio Stream (Tx)| STT
    TTS --> |2. Audio Stream (Rx)| AI_Connector
```

## 4. 핵심 컴포넌트 동작 방식 (Core Components)

### 4.1 SIP Endpoint (세션 관리)
*   **역할:** PBX로부터 들어오는 `INVITE` 콜을 수락하고(Answer), 통화 종료(`BYE`) 시 세션을 정리합니다. 콜봇 솔루션 자체가 하나의 전화기(User Agent)처럼 동작합니다.

### 4.2 RTP Media Port & Transcoder (미디어 브릿지)
*   **역할:** 콜봇의 핵심 컴포넌트입니다.
    1.  **Rx (수신):** PBX에서 들어오는 RTP 패킷(보통 G.711 8kHz)을 디코딩하여 원시 오디오 형태(PCM 16kHz 등 AI 엔진이 요구하는 포맷)로 변환합니다.
    2.  **Tx (송신):** TTS로부터 받은 오디오 스트림(PCM)을 다시 G.711 RTP 패킷으로 인코딩하여 PBX로 송출합니다.
    3.  **VAD (Voice Activity Detection):** Silero VAD v4 모델을 사용하여 게이트웨이 자체적으로 음성 구간을 감지합니다. 20ms(320샘플) 단위의 RTP 데이터를 32ms(512샘플) 단위로 리샘플링/버퍼링하여 ONNX 추론을 수행, 정확도를 극대화합니다.

### 4.3 AI Stream Connector (STT/TTS 연동 스레드)
*   **역할:** 변환된 오디오 프레임 버퍼를 gRPC Bi-directional Streaming을 통해 STT로 실시간 스트리밍합니다. 
*   **Barge-in 제어:** AI 엔진으로부터 `END_OF_TURN` 메시지와 함께 `clear_buffer=true` 플래그를 수신하면, 게이트웨이 내부의 TTS RingBuffer를 즉시 비우고(Flush) 송출을 중단합니다. 이를 통해 사용자의 발화 직후 AI의 답변이 즉각적으로 멈추는 자연스러운 대화 경험을 제공합니다.

## 5. 핵심 데이터 흐름 (Core Data Flow)

### 5.1 VAD 및 오디오 처리 파이프라인
게이트웨이 내부에서 오디오 스트림은 다음과 같은 단계로 처리되어 실시간 VAD 및 통신을 수행합니다.

1.  **SIP RTP 수신**: PBX로부터 20ms 간격으로 8kHz G.711(PCMA/PCMU) RTP 패킷을 수신합니다.
2.  **원시 오디오 추출**: G.711 데이터를 디코딩하여 8kHz PCM 데이터로 변환합니다.
3.  **인스턴트 리샘플링 (SpeexDSP)**: AI 엔진(STT)과 Silero VAD 로직을 위해 8kHz PCM을 16kHz PCM으로 실시간 업샘플링합니다.
4.  **VAD 버퍼링 및 추론 (ONNX)**: 16kHz PCM 데이터를 512 샘플(32ms) 단위로 내부 버퍼링하여 ONNX 런타임에서 Silero VAD 추론을 수행, 화자 발화 여부(`is_speaking`)를 판단합니다.
5.  **gRPC 스트리밍 (AI 전송)**: 버퍼링된 16kHz 오디오 데이터와 VAD 추론 결과(`is_speaking`)를 `AudioChunk`에 담아 AI 엔진(STT 등)으로 스트리밍 전송합니다.

## 6. 특화 성능 및 핵심 아키텍처 (Technical Considerations)

1.  **gRPC Channel Pooling (채널 공유)**: `AppConfig` 싱글톤을 활용하여 게이트웨이 시작 시 단일 gRPC 채널을 생성하고 모든 콜이 이를 공유합니다. 이는 매 통화마다 발생하는 값비싼 TCP/TLS 핸드셰이크를 제거하여, 100콜 이상의 대규모 동시 접근 시에도 병목 없이 안정적인 AI 스트리밍을 제공합니다.
2.  **통합 설정 가시화 (AppConfig)**: PJSIP의 딥 콜백(Deep Callback) 체인 내부에서 산발적으로 호출되던 `getenv()` 시스템 콜을 모두 제거하고, 시작 시 `AppConfig`에 1회 캐싱하여 읽기 전용으로 접근함으로써 Hot-path 블로킹 위험을 차단했습니다.
3.  **UUID v4 기반 세션 트레이싱**: PJSIP에서 재사용되는 정수형 Call-ID 대신 UUID 기반의 고유 Session ID를 채택하여, 클라우드 분산 환경(ELK/Grafana)에서 콜 생애주기(Call Lifecycle)부터 AI 추론 로그까지 일관되게 추적(Tracing)할 수 있습니다.
4.  **Zero-Copy RingBuffer:** `std::mutex`와 `std::condition_variable`을 활용한 커스텀 RingBuffer를 통해 미디어 워커 스레드와 gRPC 스트리밍 스레드 간의 데이터 교환 오버헤드를 최소화합니다.
5.  **Fault Tolerance:** gRPC 스트림 단절 시 `VoicebotAiClient`에서 감지하여 콜 세션을 안전하게 연결 해제하거나 백오프 재연결을 시도하는 로직이 상위 `VoicebotCall` 레벨에서 관리됩니다.
6.  **Resource Management:** `SessionManager`를 통해 동시 호 수를 논리적으로 제한하여 CPU 및 메모리 자원의 고갈(OOM)을 방지합니다. (Default Max: 100 Calls)
