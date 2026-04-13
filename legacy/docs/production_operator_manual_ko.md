# Voicebot Gateway (VBGW) 프로덕션 운영자 메뉴얼 (Operator Manual)

본 문서는 VBGW를 프로덕션 환경(Kubernetes, 리눅스 서버 등)에 배포하고 유지보수하며, 모니터링을 책임지는 **SRE, 시스템 운영자, DevOps 엔지니어**를 위한 매뉴얼입니다.

---

## 1. 배포 및 필수 환경 설정 (Deployment & Config)

VBGW는 환경 변수(`.env`)에 의해 동작이 결정됩니다. 프로덕션 환경(Docker, K8s ConfigMap)에서 **반드시** 체크해야 할 설정은 다음과 같습니다.

### 1.1 필수 환경 변수
- `VBGW_PROFILE=production`: 보안 및 운영 검증 모드를 켭니다. (이 모드에서는 인증과 TLS 등에 대한 기준이 엄격해집니다.)
- `SIP_TRANSPORT_TLS_ENABLE=1`, `SRTP_MANDATORY=1`: 네트워크 도청 방지
- `GRPC_USE_TLS=1`: 백엔드 AI 서버와의 gRPC 보안 연동
- `ADMIN_API_KEY`: 외부 노출되지 않도록 K8s Secret 등으로 주입 요망.

### 1.2 RTP 미디어 포트 (네트워크 & 방화벽)
- `RTP_PORT_MIN=16000`, `RTP_PORT_MAX=16500`: 방화벽 및 보안그룹(Security Group)에서 해당 UDP 포트 범위가 개방되어 있어야 합니다.
- (중요) Docker/K8s 이용 시 브릿지 네트워킹은 패킷 제약이 있을 수 있으니 `hostNetwork: true`를 권장합니다.

---

## 2. 관제 및 모니터링 (Observability)

VBGW는 포트 `8080`(기본값)을 통해 Prometheus 수집용 텍스트 포맷과 상태 API를 노출합니다.

### 2.1 Health Check (Liveness/Readiness)
Load Balancer 타겟 매핑 및 K8s Probe 설정 시 아래 엔드포인트를 사용하세요.
- `GET /live`: 데몬 프로세스가 살아있는지 검사 시 사용 (실패 시 즉각 Restart)
- `GET /ready`: SIP 수발신 여부, PJSIP 계정 생성 준비상태 확인 (Ready가 안 되면 트래픽 인입 금지)

### 2.2 메트릭 대시보드 (Metrics)
Prometheus로 수집 가능한 주요 지표(`GET /metrics`)입니다. 통계 이탈 발현 시 즉각 Slack 알럿을 구성할 것을 권장합니다.
1. `vbgw_active_calls`: 현재 통화 중인 호 수 (급감/급증 알람 요망)
2. `vbgw_grpc_stream_errors_total`: AI 서버와 통신 실패 시 카운트 증가. 
3. `vbgw_rtp_rx_lost_total`: 네트워크 혼잡도 측정 지표 (값 급증 시 SBC/방화벽 체크)

---

## 3. 상용 트러블슈팅 및 장애 대응 (SOP)

### 3.1 디스크 풀(Disk Full) 예방 정책
디스크가 꽉 차 서버가 다운되는 것을 막기 위한 자동 삭제 장치가 있습니다.
- 설정: `CALL_RECORDING_MAX_MB=10240` (기본 10GB 설정 권장), `CALL_RECORDING_MAX_DAYS=30`
- 동작 기작: 데몬 스레드가 백그라운드에서 1시간마다 확인하여 초과된 분량의 오래된 녹음(.wav) 파일부터 **자동 영구 삭제**합니다.

### 3.2 서버 종료(무중단 셧다운) 정책 가이드
VBGW는 **Graceful Shutdown**을 지원합니다.
- 조작 1: 터미널에서 `kill -INT <PID>`를 치거나 K8s가 `SIGTERM`을 발송합니다.
- 절차:
  1. 외부로부터의 **신규 API 콜 수신을 즉시 거부** (HTTP Server 종료).
  2. 현재 진행 중인 통화를 대상자에게 무중단 통보 (`SIP BYE` 절차 수행).
  3. 모든 연결 해제 소요 시간(최대 약 3초) 대기 후 데몬 안전 파괴.
- **Segfault 방어**: VBGW 내부 컴포넌트(SessionManager → Account → Endpoint)간 해제 순서를 철저히 보장하므로 `Exit Code 0`로 깔끔하게 떨어집니다. 강제 프로세스 Kill(`kill -9`)은 절대 피해주세요.

### 3.3 통화 중 AI 무응답 이슈 대응
만약 VBGW가 살아있는 상태에서 TTS(응답)만 출력되지 않는다면 다음을 체크하세요.
1. `mock_server`나 `AI_ENGINE_ADDR` 컨테이너의 CPU/OOM 현상 검사
2. (중요) `logs/vbgw.log` 내에 `Invalid input name: h` 에러가 발생했다면 VAD 모델 버전 불일치이므로 ONNX Model v5 바이너리 교체가 필요합니다.
