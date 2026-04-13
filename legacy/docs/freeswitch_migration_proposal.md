# FreeSWITCH 에코시스템 기반 AI Gateway 마스터 이관 설계서

본 문서는 현재 C++ PJSIP 기반 VBGW 프로덕션 환경에서 가동 중인 **모든 코어 기능 및 제어 인터페이스의 누락 없는 100% 치환**을 보장하도록, 더욱 정밀하게 보강된 FreeSWITCH(FS) 기반 마스터 아키텍처 설계 및 구축 마일스톤 제안서입니다.

---

## 1. 아키텍처 토폴로지 (To-Be Architecture)

기존 "단일 모놀리식 구조"에서 역할을 크게 3가지로 분리합니다. 미디어 처리는 C 커널 레벨의 FS가, 지능형 로직 제어는 Orchestrator가 전담합니다.

```mermaid
graph TD
    User[엔드 포인트 / PBX망] <==>|SIP/RTP, 멀티코덱, Trunk| FS

    %% 계층 1: FreeSWITCH Media & SIP Core
    subgraph "1. 미디어 통신 스위칭 엔진 (FreeSWITCH Core)"
        FS(FreeSWITCH)
        mod_sofia(mod_sofia<br/>SIP/보안)
        mod_audio_fork(mod_audio_fork<br/>AI 스트리밍)
        mod_event_socket(mod_event_socket<br/>상태 제어 소켓)
        DSP(Core DSP<br/>Jitter, AGC, Denoise)
        
        FS --- mod_sofia
        FS --- mod_audio_fork
        FS --- mod_event_socket
        FS --- DSP
    end

    %% 계층 2: Orchestrator (Backend App)
    subgraph "2. 통화 상태 오케스트레이터 및 API 계층"
        Orchestrator(Call Orchestrator <br/>Node.js 또는 Go / Python)
        AdminAPI[HTTP Admin API<br/>Outbound, Bridge, Transfer]
        
        AdminAPI --- Orchestrator
    end

    %% 계층 3: AI Engine
    subgraph "3. AI 서빙 계층 (기존망 호환)"
        AI(AI / NLU Engine<br/>gRPC, ONNX VAD, STT, TTS)
    end

    %% 데이터 흐름
    mod_event_socket <.->|TCP 비동기 통화 생성/해제 이벤트 제어| Orchestrator
    AdminAPI -.->|백엔드 서비스망 연동| 3rdParty[자사 3rd Party 웹 서버]
    
    mod_audio_fork == "Raw L16 PCM 스트림" ==> |Websocket/gRPC| AI
    AI -.-> |명령 콜백| Orchestrator
```

---

## 2. 프로덕션 기능 100% 반영: As-Is vs To-Be 기능 맵핑

현재 C++ 버전에 존재하는 단 하나의 기능도 빠짐없이 FreeSWITCH 토폴로지 상에 구현되는 방안을 대조했습니다.

### 📞 2.1 SIP 엣지 및 통화 생성 기능
| 현재 기능 (As-Is) | FreeSWITCH 대체 방안 (To-Be) | 비고 |
|---|---|---|
| PBX 모드 vs Local 모드 (`AppConfig`) | `sofia.conf.xml`의 Internal / External 프로파일 설정 하나로 동시 구동 및 이원화 완벽 처리 | FS는 여러 네트워크 인터페이스 동시 수용 가능 |
| SIP TLS / SRTP 필수 강제화 | Sofia 프로파일 내 `tls-only=true` 및 `rtp-secure-media=mandatory` 설정으로 내장 지원 | PJSIP보다 보안 계층이 성숙함 |
| HTTP `/api/v1/calls` (아웃바운드 콜 자동 발신) | Orchestrator가 `bgapi originate` 또는 `ESL 연결`을 통해 비동기로 동시 발신 명령 전송 | C++ 로직 통째 대체, 지연 없음 |

### 🛠️ 2.2 미디어 전처리 및 제어 (Audio & AI)
| 현재 기능 (As-Is) | FreeSWITCH 대체 방안 (To-Be) | 비고 |
|---|---|---|
| G.711 수신 -> 16kHz PCM 변환 | FS 코어가 자동으로 채널 샘플레이트를 16K/32K 등으로 트랜스코딩 | - |
| `SpeexDSP` 기반 Denoise & AGC | FS 다이얼플랜에서 `agc`, `noise_gate`, `mod_webrtc` 세팅으로 적용 가능 처리 | - |
| `VoicebotAiClient` (gRPC 양방향 스트리밍) | 통화 시작 시 다이얼플랜에서 `mod_audio_fork`(w/ Websocket) 또는 FS용 gRPC 모듈 구동하여 전송 | VBGW의 고질적 복사 오버헤드 제거 |
| `Silero VAD` 기반 발화 감지 처리 | 오디오 스트림(FS->AI) 앞단에 현재처럼 배치하거나, FS의 내장 VAD 모듈 스위치 활용 | - |
| 말끊기 시 TTS 링버퍼 플러시 (Barge-In) | 발화 감지 시 오케스트레이터가 FS에 `uuid_break <콜ID>`를 날리면 현재 재생 중인 TTS가 즉각 깔끔하게 소산됨 | 동기화 이슈 원천 차단 |

### 🔀 2.3 실시간 부가 제어 로직 (Call Routing API)
| 현재 기능 (As-Is) | FreeSWITCH 대체 방안 (To-Be) | 비고 |
|---|---|---|
| 콜 브릿지 및 연결 해제 API (상담원 연결) | `uuid_bridge <유저> <상담원>` / 분리는 `uuid_transfer` API로 1초 이내 즉각 라우팅 | - |
| 상담원 연결 시 AI 스트림 전송 중지 | Orchestrator가 bridge 전송 직전에 `uuid_audio_fork_stop`명령을 실행해 AI 서버로의 유입 차단 | P2 기능 대치 |
| DTMF 이벤트 가로채기 및 IVR 메뉴 상태 제어 | FS 자체적으로 DTMF 이벤트를 잡아 ESL로 JSON 토스. 오케스트레이터(코드)가 직접 판단 후 행동 지시. | 교착 상태(Deadlock) 위험 영구 제거 |
| 통화 블라인드 전환(REFER) | `uuid_transfer <초기콜ID> <타겟전화번호>` 커맨드 사용 | - |

### ⚙️ 2.4 데몬 운영 및 관측성
| 현재 기능 (As-Is) | FreeSWITCH 대체 방안 (To-Be) | 비고 |
|---|---|---|
| 실시간 통화 녹취 (Start / Stop) | `uuid_record <콜ID> start <경로>` / `stop` 명령으로 파일 저장 지원 | - |
| 디스크 용량 (Quota/Retention) 스케줄 관리 | 리눅스의 `Crontab`이나 Orchestrator 데몬 내 주기적 `fs.unlink()` 루핑으로 관리 (기존 코드 로직 이관) | P2 기능 대치 |
| Liveness, Readiness, Prometheus Metrics | Orchestrator 내장 Exporter, 또는 FS용 외부 Prometheus Exporter 활용 체계 구축 | 기존 지표 100% 이관 |
| Graceful 셧다운 (Segment Fault 방어) | FS 내부 내장 명령 `fsctl pause` 후 프로세스 종료 시 잔여 통화 유지 메커니즘 지원 | P0 기능 대치 |

---

## 3. 안정화 마이그레이션 및 구축 마일스톤 

이관 프로젝트는 **현재 구동 중인 시스템의 중단과 고객 혼선을 방지**하기 위해 단계적(Phased)으로 오픈하는 전략을 취해야 합니다.

### Phase 1: 기반 미디어 인프라 PoC (1~2주)
- **목표**: FreeSWITCH 컨테이너 베이스에 `mod_audio_fork` 및 필수 SIP/RTP 설정 구축.
- **작업**: 
  - PBX 연동 테스트 및 16kHz 오디오 스트리밍을 AI Mock 서버와 Websocket(또는 gRPC)으로 주고받는 뼈대 증명.
  - VAD 감지 시 FS 서버로 `uuid_break(말끊기)` 제어 명령이 안정적인 RTT(지연시간)로 도달하는지 수동 테스트 진행.

### Phase 2: Orchestrator Application 개발 (3~4주)
- **목표**: 기존 C++ 비즈니스 로직(IVR, 브릿지, HTTP API)을 새로운 오케스트레이터로 전면 교체 개발.
- **작업**:
  - `Node.js` (또는 `Go`, `Python`)를 이용해 ESL(Event Socket Library) 클라이언트 개발.
  - 기존 `HttpServer.cpp`에 구현된 모든 `/api/v1/calls` 엔드포인트를 동일한 URL 체계와 응답값(Response Body)을 갖는 API로 복제 개발. (하위 호환성 보장)
  - 디스크 쿼터 청소 로직(Cron), 헬스체크 구현.

### Phase 3: 무중단 검증, E2E 병행 테스트 (2주)
- **목표**: SIP 부하 시뮬레이터를 통한 대용량 스트레스 확인.
- **작업**:
  - Sipp, JMeter 등을 이용하여 동시 다발 500~1000콜을 발신.
  - AI 세션 연결 중 Memory Leak 추이 비교 관찰. (C++ 대비 훨씬 안정된 가용량 확보 검증)

### Phase 4: 상용 배포 및 Blue/Green 전환 (1주)
- 기존 PBX의 라우팅 룰을 기존 C++ 게이트웨이는 살려둔 채, 10%의 트래픽만 새로운 인프라로 넘겨봅니다.
- 1주일 모니터링 후, 에러율이 0%에 수렴하면 전면 100% 트래픽 라우팅 처리.
