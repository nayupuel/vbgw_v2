# Voicebot Gateway (VBGW) 통합 환경 테스트 케이스 명세서

본 문서는 상용 콜 인프라 연동망과 동일한 수준의 품질을 보장하기 위한 8대 핵심 기능의 시나리오 검증(QA) 목록입니다. 각 항목마다 시스템 동작 로그와 에뮬레이터 지표를 기준으로 Pass/Fail을 심사합니다.

## TC-01: SIP 시그널링 및 세션 라이프사이클 (SIP Call Setup & Teardown)
- **개요:** PBX 연동 또는 Direct IP 환경에서 SIP INVITE를 안정적으로 수신하고 종료(BYE)하는지 확인.
- **사전 조건:** VBGW 프로세스 구동 (Local 모드 또는 PBX 레지스트레이션 완료 상태).
- **테스트 단계:**
  1. SIP 에뮬레이터를 통해 VBGW(`sip:voicebot@127.0.0.1:5060`)로 INVITE 발송.
  2. 200 OK 수신 및 RTP 협상 완료 확인.
  3. 5초 대기 후 에뮬레이터 측에서 BYE 발송.
- **기대 결과:** 메모리 누수 없이 `pj::Call` 자원이 해제되고 정상 종료.

## TC-02: RTP 미디어 품질 및 Speex 처리 (Media Quality & Speex DSP)
- **개요:** 8kHz의 외부 G.711(PCMA/PCMU) 코덱 스트림이 16kHz PCM으로 성공적으로 변환되고, 스피치 노이즈 제어(SpeexDSP)를 거치는지 확인.
- **사전 조건:** TC-01 통과, 유효한 미디어 스트림 입력.
- **테스트 단계:**
  1. 더미 오디오 버퍼(또는 `.wav` 파일)를 RTP 스트림으로 2초 이상 전송.
  2. VBGW 프로세스 콘솔(또는 로그)에서 `SpeexDSP` 초기화 및 AGC/Denoise 개입 유무 확인.
- **기대 결과:** RTP Drop이나 Buffer Underrun 경고 없이 온전한 16kHz 리샘플 처리가 달성됨.

## TC-03: Silero VAD 민감도 및 gRPC 연동 (AI Streaming)
- **개요:** 사일런스(침묵)가 아닌 실제 목소리가 섞인 프레임을 받았을 때, VAD v5 모델이 이를 정확히 판단하고 gRPC를 통해 AI서버로 전송하는지 판별.
- **사전 조건:** Mock AI 서버(`mock_server.py`)가 50051 포트로 대기 중.
- **테스트 단계:**
  1. 에뮬레이터에서 Call 실행 및 오디오 프레임 전송 개시.
  2. Mock AI Server 콘솔의 수신 로그(`is_speaking=True/False` 비율) 확인.
- **기대 결과:** VBGR 로그 채널에 `VAD 추론 오류(h_state)`가 발생하지 않으며, AI서버가 원만히 데이터를 수신함.

## TC-04: TTS 반응 및 말끊기 처리 (Barge-in / TTS)
- **개요:** AI가 TTS 음성을 VBGW 링버퍼로 내려보낼 때, 사용자의 인터럽트(Barge-in)가 발생했을 시 남은 버퍼 잔량이 올바르게 폐기되는지 확인.
- **사전 조건:** 콜 연결 완료, Mock AI 서버에서 지속적인 TTS Chunk를 응답 중.
- **테스트 단계:**
  1. AI 서버에서 역으로 `clear_buffer=true` 속성이 담긴 Barge-in 신호(또는 END_OF_TURN) 발송 시뮬레이션.
  2. 게이트웨이 내 `VoicebotMediaPort::clearTtsAudio()` 실행 로그 확인.
- **기대 결과:** 링버퍼 플러시 로그(`flush`) 확인 후 재생되던 RTP가 즉시 묵음 처리됨.

## TC-05: IVR 상태머신 및 DTMF 이벤트 처리 (IVR & DTMF)
- **개요:** DTMF 키패드 입력 시그널을 수신하여 `MENU`, `AI_CHAT`, `TRANSFER`, `DISCONNECT` 모드 간 상태 전이를 올바르게 처리하는지 확인.
- **사전 조건:** 통화 연결 (상태 파이프 활성).
- **테스트 단계:**
  1. 에뮬레이터를 이용해 DTMF 시그널 전송: `1` → `2` → `0` → `*` → `#`.
  2. VBGW 로그의 `[IVR]` 접두사 상태 변화 스레드 추적.
- **기대 결과:** 콜백 처리가 뮤텍스 외부에서 이뤄져 deadlock(멈춤 현상)이 발생하지 않고 상태 전이 완료.

## TC-06: 통화 브릿지 및 AI 묵음화 (P2-2: Bridge Pausing)
- **개요:** `bridgeWith()` 호출(상담원 연결 시뮬레이션) 시 AI 엔진이 불필요하게 가동되지 않도록 `pause`되는지 확인.
- **사전 조건:** 통화 중 브릿지 커맨드(API) 수신.
- **테스트 단계:**
  1. 통화 상태 진입.
  2. 브릿지 모드 API(또는 내부 테스트 훅) 트리거.
- **기대 결과:** `[MediaPort] AI stream forwarding PAUSED` 로그 출력, AI 서버 오디오 수신 중단(0 bytes). 언브릿지 시 다시 `RESUMED`.

## TC-07: 녹음 정책 및 콜 디스크 할당 (P2-1: Quota)
- **개요:** `AppConfig`에 명시된 할당량(Days, MB)에 도달했을 때, 주기적 클리너 함수가 오래된 녹음 파일을 올바르게 삭제하는지 확인.
- **사전 조건:** `recordings` 폴더 생성 후, 파일 수정 날짜가 30일 이전인 가짜 `.wav` 파일 생성.
- **테스트 단계:**
  1. VBGW 실행 시 초기 `cleanUpRecordings` 동작.
  2. VBGW 콘솔 출력 로그 열람.
- **기대 결과:** `Removed old recording` 메시지가 출력되며, 가짜 파일 강제 삭제 결과 확인.

## TC-08: 안정적 데몬 종료 시퀀스 (Graceful Shutdown)
- **개요:** 동시 전화 다수가 물려 있는 상황에서 SIGINT(Ctrl+C) 신호를 받았을 때 충돌(Segfault) 없이 깨끗하게 종결되는지 확인.
- **사전 조건:** 2~3개의 복수 Call 동시 수신/유지 중.
- **테스트 단계:**
  1. `kill -INT <P_ID>` 전송.
  2. `[Shutdown 1/5]` ~ `[Shutdown 5/5]` 콘솔 추적.
- **기대 결과:** 통화 세션이 차례로 `PJSIP_SC_DECLINE`/`BYE` 해제되며 Segfault 없이 0번 Exit 코드로 메인 프로세스 종료.
