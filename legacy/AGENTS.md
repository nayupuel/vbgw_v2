# AGENTS.md — VoiceBot Gateway (vbgw)

> 이 파일은 Codex가 매 대화 시작 시 자동으로 읽는 프로젝트 지시서입니다.

---

## 1. 프로젝트 개요

**VoiceBot Gateway (vbgw)** — AI 콜봇 인프라의 핵심 통화 제어 및 미디어 게이트웨이

| 항목 | 내용 |
|------|------|
| 역할 | PBX/SBC로부터 SIP 전화를 수신하여 AI 엔진(STT/TTS/NLU)과 실시간 음성 스트리밍 중계 |
| 언어 | C++20 |
| 빌드 | CMake 3.15+ / Ninja |
| 핵심 라이브러리 | PJSIP, Boost.Asio, gRPC, protobuf, ONNX Runtime, SpeexDSP, spdlog, OpenSSL |
| 프로토콜 | SIP (시그널링), RTP/G.711 (미디어), gRPC Bi-directional Streaming (AI 연동) |
| VAD 모델 | Silero VAD v4 (models/silero_vad.onnx) |

---

## 2. 디렉토리 구조

```
vbgw/
├── src/
│   ├── main.cpp                    # 진입점, 설정 로드, PJSIP 초기화
│   ├── engine/                     # SIP/미디어 핵심 엔진
│   │   ├── VoicebotEndpoint.cpp/h  # PJSIP UA — SIP 등록/인증/라우팅
│   │   ├── VoicebotAccount.cpp/h   # SIP 계정 관리
│   │   ├── VoicebotCall.cpp/h      # 콜 세션 오케스트레이터 (핵심)
│   │   ├── VoicebotMediaPort.cpp/h # RTP 수신·VAD·gRPC 송신 파이프라인
│   │   └── SessionManager.h        # 동시 호 수 관리 (Max 100)
│   ├── ai/
│   │   ├── VoicebotAiClient.cpp/h  # gRPC Bi-dir 스트리밍 클라이언트
│   │   └── SileroVad.cpp/h         # ONNX Runtime 기반 VAD 추론
│   ├── utils/
│   │   └── RingBuffer.h            # Zero-copy lock-free 링버퍼
│   └── emulator/                   # Python AI 서버 목 (테스트용)
│       ├── mock_server.py          # gRPC 목 서버
│       └── emulator.py             # SIP 콜 에뮬레이터
├── protos/
│   └── voicebot.proto              # gRPC 인터페이스 정의 (원본)
├── models/
│   └── silero_vad.onnx             # VAD 모델 (바이너리, 수정 금지)
├── config/                         # 런타임 설정 (환경별)
├── docs/
│   ├── architecture.md             # 시스템 아키텍처 설계서
│   ├── api_spec.md                 # gRPC API 명세
│   └── troubleshooting.md         # 운영 트러블슈팅 가이드
├── build/                          # CMake 빌드 산출물 (무시)
└── .Codex/
    ├── agents/                     # 페르소나 에이전트 10개
    └── commands/                   # 커스텀 슬래시 커맨드
```

---

## 3. 빌드 및 실행 명령어

### 빌드
```bash
# 처음 설정 (빌드 디렉토리 생성)
cmake -S . -B build -G Ninja

# 빌드 실행
cmake --build build

# 또는 직접
cd build && ninja
```

### 실행
```bash
./build/vbgw
```

### Emulator (테스트용 Python AI 목 서버)
```bash
# 목 gRPC AI 서버 실행
cd src/emulator && python mock_server.py

# SIP 콜 에뮬레이터 실행
cd src/emulator && python emulator.py
```

### Protobuf 재생성 (proto 파일 변경 시)
```bash
# CMake가 자동 처리 — 빌드 시 build/generated/ 에 생성됨
cmake --build build
```

---

## 4. 핵심 데이터 흐름

```
PBX/SBC
  │ SIP INVITE
  ▼
VoicebotEndpoint → VoicebotAccount → VoicebotCall (오케스트레이터)
                                           │
                                           ▼
                                    VoicebotMediaPort
                                    ┌─────────────────────────────────┐
                                    │ RTP G.711 수신 (8kHz)           │
                                    │ → SpeexDSP 리샘플링 (16kHz)     │
                                    │ → SileroVAD 추론 (512샘플/32ms) │
                                    │ → AudioChunk 조립               │
                                    └──────────────┬──────────────────┘
                                                   │ gRPC Bi-dir Stream
                                                   ▼
                                          VoicebotAiClient
                                          (STT/TTS/NLU 서버)
                                                   │ AiResponse
                                                   ▼
                                    RingBuffer → RTP 인코딩 → PBX 송출
```

---

## 5. gRPC 인터페이스 요약

```protobuf
// protos/voicebot.proto
service VoicebotAiService {
    rpc StreamSession(stream AudioChunk) returns (stream AiResponse);
}

// Gateway → AI: 20ms 오디오 청크
message AudioChunk {
    string session_id = 1;  // SIP Call-ID
    bytes audio_data = 2;   // 16kHz PCM (20ms)
    bool is_speaking = 3;   // VAD 결과
}

// AI → Gateway: STT 텍스트 또는 TTS 오디오
message AiResponse {
    enum ResponseType { STT_RESULT=0; TTS_AUDIO=1; END_OF_TURN=2; }
    ResponseType type = 1;
    string text_content = 2;
    bytes audio_data = 3;
    bool clear_buffer = 4;  // Barge-in 시 RingBuffer 플러시
}
```

---

## 6. 코딩 컨벤션

### C++ 스타일
- **표준:** C++20
- **네이밍:** 클래스 `PascalCase`, 멤버변수 `snake_case_`, 함수 `camelCase`
- **스마트 포인터:** raw pointer 대신 `std::unique_ptr` / `std::shared_ptr` 사용
- **RAII 필수:** 모든 리소스(PJSIP, gRPC 채널, 파일 핸들)는 RAII로 관리
- **스레드 안전:** 공유 자원은 반드시 `std::mutex` 또는 `std::atomic` 보호
- **에러 처리:** 에러를 삼키지 않음 — spdlog로 로깅 후 상위로 전파
- **헤더 가드:** `#pragma once` 사용

### 금지 사항
- `new` / `delete` 직접 사용 금지 (스마트 포인터 대체)
- 하드코딩된 IP/포트/경로 금지 (config 파일 또는 환경변수)
- `using namespace std;` 전역 선언 금지
- `models/silero_vad.onnx` 파일 수정 금지 (바이너리)

---

## 7. 페르소나 에이전트 사용 가이드

이 프로젝트에는 `.Codex/agents/` 에 10개의 전문 페르소나가 설정되어 있습니다.

### 팀 구성
| 에이전트 | 레벨 | 1차 역할 | 레드팀 대상 |
|----------|------|----------|-------------|
| `planner` | 시니어 | 기획/요구사항 | verifier, tester, docs |
| `architect-system` | 전문가 | 시스템 아키텍처 | planner, developer-* |
| `architect-software` | 전문가 | 소프트웨어 설계 | developer-*, architect-data |
| `architect-data` | 시니어 | 데이터/프로토콜 설계 | developer-integration, docs |
| `developer-core` | 시니어 | C++ 핵심 구현 | architect-*, tester |
| `developer-integration` | 시니어 | gRPC/AI 연동 | architect-data, tester, verifier |
| `verifier-requirements` | 시니어 | 요구사항 검증 | planner, architect-* |
| `verifier-code-review` | 전문가 | 코드 품질/보안 감사 | developer-*, architect-software |
| `tester-qa` | 시니어 | 테스트 설계/QA | developer-*, architect-* |
| `documentation-writer` | 시니어 | 산출물 작성 | 전체 팀 산출물 |

### 호출 예시
```
"developer-core로 VoicebotCall 버그 수정해줘"
"architect-software가 레드팀으로 코드 설계 원칙 검토해줘"
"verifier-code-review가 src/ai/VoicebotAiClient.cpp 전체 감사해줘"
```

---

## 8. 주의사항 및 알려진 이슈

- `build/` 폴더는 `.claudeignore`로 제외 — CMake 빌드 산출물
- `build/generated/voicebot.pb.*` 는 proto 컴파일 자동 생성 파일 — 직접 수정 금지
- `models/silero_vad.onnx` 는 바이너리 파일 — 읽기/분석 불필요
- PJSIP 콜백은 PJSIP 내부 스레드에서 실행됨 — UI 스레드나 다른 스레드와 동기화 주의
- gRPC Completion Queue는 별도 스레드에서 폴링 — VoicebotAiClient 생명주기 주의

---

## 9. 관련 프로젝트 (모노레포)

```
/Users/kchul199/Desktop/project/antigravity_project/
├── vbgw/    ← 현재 프로젝트 (C++ VoiceBot Gateway)
├── apigw/   ← Open API Gateway (Python/FastAPI, K8s)
└── first_proj/ ← 프론트엔드 프로토타입 (HTML/JS)
```
