# Voicebot Gateway (VBGW) Development History

## 개요
이 문서는 C++ 및 PJSUA2(PJSIP) 기반으로 구축된 AI Voicebot Gateway(콜봇 게이트웨이)의 전체 개발 이력 및 주요 의사결정 사항들을 시간순으로 기록한 문서입니다. PBX 연동, STT/TTS 양방향 스트리밍, 그리고 Edge AI(Siler VAD) 탑재까지의 모든 과정을 담고 있습니다.

---

## 2026-04-06

- `[34ffa9c]` **feat: [Day-2] production hardening - RTP scaling, SBC trunking, and daily log rotation** — 2026-04-06 23:46 (5 파일, +93/-14)
  - 변경: Dockerfile,docs/optimization_roadmap.md,history/development_history.md,src/main.cpp,src/utils/AppConfig.h
- `[3354c2e]` **feat: 운영 안정성 강화 — PBX/SBC 연동 중심** — 2026-04-06 22:48 (35 파일, +2865/-194)
  - 변경: .dockerignore,.github/workflows/ci.yml,.github/workflows/e2e-outbound-null-audio.yml,AGENTS.md,CMakeLists.txt,Dockerfile,README.md,docs/slo_runbook.md,docs/testing.md,docs/troubleshooting.md
## 2026-03-29

- `[83371ba]` **feat(arch): implement Phase 2 & 3 architecture revamps** — 2026-03-29 22:48 (12 파일, +513/-109)
  - 변경: CMakeLists.txt,README.md,history/development_history.md,src/api/HttpServer.cpp,src/api/HttpServer.h,src/engine/VoicebotCall.cpp,src/engine/VoicebotCall.h,src/engine/VoicebotEndpoint.cpp,src/engine/VoicebotEndpoint.h,src/main.cpp
- `[d8c57b8]` **build: CMake 멀티플랫폼 대응 및 docs 현행화** — 2026-03-29 22:29 (3 파일, +28/-9)
  - 변경: CMakeLists.txt,docs/architecture.md,docs/troubleshooting.md
- `[5e3f664]` **docs: 아키텍처 리뷰 보고서 추가** — 2026-03-29 22:12 (1 파일, +649/-0)
  - 변경: docs/code_review_report.md
- `[ea3728b]` **chore: 저장소 정리 및 개발 이력 업데이트** — 2026-03-29 22:10 (42 파일, +12/-10696)
  - 변경: .cache/clangd/index/SileroVad.cpp.8F7964E92BC49AED.idx,.cache/clangd/index/SileroVad.h.9DBB45CB0AEEF5A8.idx,.cache/clangd/index/VoicebotAccount.cpp.B6EFA9DB5817B1A0.idx,.cache/clangd/index/VoicebotAccount.h.D876F23E07DAEE4B.idx,.cache/clangd/index/VoicebotAiClient.cpp.0175DC5F1297F6E4.idx,.cache/clangd/index/VoicebotAiClient.h.A5DDB2D3888C3675.idx,.cache/clangd/index/VoicebotCall.cpp.D9B50BB8CED1AA2D.idx,.cache/clangd/index/VoicebotCall.h.45881163B75E250F.idx,.cache/clangd/index/VoicebotEndpoint.cpp.7696CA2B6A8E1BA5.idx,.cache/clangd/index/VoicebotEndpoint.h.8F0802F91652FBFC.idx
- `[5774726]` **refactor: 아키텍처 리뷰 Critical~Low 20건 이슈 개선** — 2026-03-29 22:08 (25 파일, +2013/-433)
  - 변경: .clang-format,.gitignore,CLAUDE.md,Dockerfile,docs/architecture.md,docs/testing.md,protos/voicebot.proto,src/ai/SileroVad.cpp,src/ai/SileroVad.h,src/ai/VoicebotAiClient.cpp
## 2026-03-22

- `[9f64764]` **Docs: enhance README.md to be highly detailed and beginner-friendly** — 2026-03-22 22:36 (1 파일, +117/-80)
  - 변경: README.md
## 📅 프로젝트 개발 연혁

### 1단계: 기본 인프라 및 핵심 엔진 구축 (Phase 1)
- **개발 환경 구성:** macOS(M칩) 및 Homebrew 환경에서 `pjsip`, `grpc`, `protobuf`, `openssl`, `cmake` 등의 핵심 라이브러리 연동 완료.
- **PJSUA2 엔진 초기화:** `VoicebotEndpoint` 클래스를 설계하여 SIP 스택의 라이프사이클(초기화 및 Graceful Shutdown) 제어 구현.
- **오디오 I/O 파이프라인 (미디어 제어):** PJSUA2의 MediaPort를 상속받은 `VoicebotMediaPort` 구현. 송수신 오디오 패킷(20ms, 16kHz, 16bit)을 핸들링하기 위해 스레드 세이프(Thread-Safe)한 `RingBuffer` 독자 구현 완료.

### 2단계: AI 양방향 라이브 스트리밍 연동 (Phase 2)
- **gRPC 프로토콜 포팅:** `voicebot.proto` 스키마 디자인 및 C++용 Stub/Protobuf 생성 스크립트 구축.
- **비동기 스트림 워커 (Read/Write):** `VoicebotAiClient`를 통해 메인 스레드 대기 현상(Blocking) 없이 오디오 데이터와 텍스트(STT/TTS 이벤트)를 양방향으로 고속 비동기 송수신.
- **동시 호 관리자 (OOM 방지):** `SessionManager` 클래스를 신설하여 Max Calls(100채널 제한) 제어. OOM(Out of Memory) 예방 및 과부하 시 `486 Busy Here` 반환 방어 로직 구현.

### 3단계: C++ Edge VAD 임베딩 및 고도화 (Phase 3)
- **ONNX Runtime (Silero VAD) 탑재:** 단순히 서버로 소리를 밀어넣는 것이 아닌, 게이트웨이 자체적으로 사람 목소리와 노이즈를 구별(VAD)하여 통신 대역폭과 STT 낭비를 최소화하는 로직 탑재.
- **512 텐서 Chunk 병합 (초정밀 감지):** SIP RTP의 기본 규격인 320 프레임(20ms)을 머신러닝 학습 기준인 512 프레임(32ms)으로 내부 버퍼에 모은 뒤 추론(Inference)을 수행, VAD 엔진 정확도를 폭발적으로 상승.
- **말끊기(Barge-in) 최적화 회로:** 화자가 말을 시작할 경우(is_speaking=true), AI 서버가 보내던 기존 TTS 버퍼들을 강제로 씻어내어(Flush) 즉각적인 리스닝 상태(VAD Reset)로 전환시켰습니다.

### 4단계: 기업용 고가용성 대응 및 PBX 연동 (Phase 4)
- **오동작 방어 콜백:** AI 모델링 서버 장애 혹은 네트워크 절단 발생 시(Disconnect), 즉각 SIP 포트에 `503 Service Unavailable` 혹은 `BYE`를 전송해 좀비 채널 생성을 방지. (Fault Tolerance)
- **동적 PBX(Asterisk/FreePBX) 자동 등록 기능:** `main.cpp` 구동 시 로컬 환경변수(`PBX_URI`, `PBX_USERNAME`, `PBX_PASSWORD` 등)를 파싱 및 주입하여, 단순 IP-to-IP 콜 외에도 엔터프라이즈급 내선망/운영망 PBX 서버 로그인이 자동 수행되도록 확장 성공.

---

## 🛠️ 핵심 성과 및 벤치마킹 요약
1. **Zero-Latency Pipelining:** PJSUA2 미디어 프레임워크와 gRPC 스트림 사이를 RingBuffer 하나로 직접 연결하여 메모리 복사 최소화 달성.
2. **Crash-Free 스레드 관리:** 콜 분할/종료나 포인터 교체 시 Data Race를 막기 위한 원자적 락킹(`std::mutex`) 기법 적용 완료.

## 📝 다음 목표 (Next Steps)
- 파이썬 Mock Server 기반 E2E STT/TTS 통합 테스트
- Docker-compose화 하여 컨테이너 기반 쿠버네티스(K8s) 배포 패키징
