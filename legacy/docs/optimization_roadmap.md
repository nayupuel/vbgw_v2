# Voicebot Gateway (VBGW) 프로덕션 기술 고도화 및 최적화 로드맵

본 문서는 콜 인프라 운영 및 아키텍처 전문가 관점에서 VBGW가 성공적인 상용화를 넘어, **초대규모 퍼포먼스(High Performance), 무결점 안정성(High Availability), 그리고 최적의 관제(Observability) 환경**을 달성하기 위한 단계별 기술 고도화 로드맵입니다.

---

## 🚀 Phase 1: 처리량 극대화 및 미디어 레이턴시 최적화 (Performance & Low Latency)

AI 콜봇의 핵심은 "사용자가 체감하는 지연 시간(End-to-End Latency)"의 최소화입니다. 100~200ms 단위의 최적화가 이뤄져야 자연스러운 대화가 가능합니다.

### 1.1 Zero-Copy 미디어 파이프라인 달성
- **현재 상태**: RTP 수신 → SpeexDSP → VAD → std::vector 복사 → gRPC 전송 과정에서 일부 메모리 복사가 발생.
- **최적화 방안**: 
  - gRPC `ByteBuffer` 파이프라인을 직접 활용하거나, Lock-free RingBuffer 연산을 개선하여 메모리 복사 오버헤드 0% 달성.
  - VAD 추론부 연산과 네트워크 송출(I/O) 스레드를 완벽히 분리하여 병목 현상 제거.

### 1.2 네트워크 Jitter & 패킷 유실 방어
- **현재 상태**: PJSIP 기본 Jitter Buffer 및 재전송 제어 의존.
- **최적화 방안**: 
  - 네트워크가 불안정한 환경(모바일 등)을 위한 PLC(Packet Loss Concealment) 최적화.
  - VAD 알고리즘 내 백그라운드 노이즈 임계치 자동 보정(Dynamic AGC/Denoise Profile) 등 SpeexDSP 심화 튜닝 적용.

### 1.3 AI 엔진 추론 분산 (TensorRT/GPU 도입)
- **현재 상태**: CPU 기반의 ONNX Runtime으로 Silero VAD 구동.
- **최적화 방안**: 
  - 대용량 트래픽 대비 C++ ONNX Runtime에 TensorRT 또는 CUDA 백엔드 적용.
  - VAD 연산(32ms 단위) 처리를 위한 고성능 워커 풀(Worker Pool) 구조 도입으로 스레드 점유율 단축.

---

## 🛡️ Phase 2: 대규모 확장성 및 이중화 아키텍처 (Scalability & HA)

엔터프라이즈 급 트래픽 처리를 위한 인프라 지향적 설계입니다. 수백~수천 채널 이상의 동시콜을 버텨내는 구조를 완성합니다.

### 2.1 SIP 트래픽 로드밸런싱 체계 구축 (Kamailio / OpenSIPS 도입)
- **현재 상태**: 단일 PBX가 게이트웨이를 1:1로 바라보는 형태.
- **최적화 방안**: 
  - 게이트웨이 앞단에 SIP Proxy(ex. Kamailio)를 전진 배치하여, 들어오는 INVITE 호를 N대의 VBGW 컨테이너 노드로 분산(Round Robin / Dispatcher). 
  - VBGW의 SessionManager 자원(CPU/Active Calls) 점유 상태를 SIP Proxy로 피드백하여 지능형 트래픽 라우팅 수행.

### 2.2 Kubernetes 기반 무중단 배포 (Zero-Downtime Deployment)
- **최적화 방안**: 
  - **Graceful Draining**: VBGW 종료(SIGTERM) 시그널 수신 시 곧바로 서버가 꺼지지 않고, 쿠버네티스의 `Readiness Probe`를 `false`로 내려 신규 콜 수신을 막되 진행 중인 통화가 끝날 때까지 대기하는 로직 심화 연동.
  - RTP 포트(UDP 16000~20000) 할당 문제를 해결하기 위해 DaemonSet 및 Host-network 체계 적용 가이드 수립.

---

## 📊 Phase 3: 초정밀 관제 및 데이터 기반 운영망 (Observability & SRE)

문제 발생 시 즉각적으로 원인을 규명(RCA)할 수 있는 가시성을 확보합니다.

### 3.1 통합 그라파나 대시보드(Prometheus/Grafana) 본격화
- **최적화 방안**: 
  - 내장된 `RuntimeMetrics` API와 Prometheus Scraper를 완전 결합.
  - **주요 지표 모니터링 체계 완비**: 동시 콜(Active Calls), Jitter Delay, gRPC 송수신 지연율(RTT), VAD 트리거 임계 성능 실시간 그래프 시각화 구축.

### 3.2 분산 트레이싱 (OpenTelemetry / Jaeger)
- **최적화 방안**: 
  - `SIP Call-ID` ↔ `VBGW Session UUID` ↔ `gRPC Trace ID` 간격을 하나로 묶어 로깅.
  - 사용자 전화 인입부터 통화 종료까지의 전 주기를 트레이스 뷰어로 볼 수 있도록 분산 트레이싱 파이프라인 구축.

### 3.3 구조화 로깅 체계(JSON Log) 연동
- **최적화 방안**: 
  - 기존 텍스트 기반 로그 출력을 ELK(Elasticsearch, Logstash, Kibana)나 FluentBit가 파싱하기 좋은 형태 통일.
  - 과다한 Debug 로그를 AI 레벨별로 샘플링(Sampling) 출력하도록 수정해 디스크 I/O 최적화.

---

## 🔒 Phase 4: 엔터프라이즈 보안 격벽 (Enterprise Security)

강력한 보안 기준을 요구하는 금융·공공 네트워크 망 투입을 위한 준비 사항입니다.

### 4.1 gRPC 및 SIP 상호 통신 보안(mTLS) 강제화
- **최적화 방안**: 
  - VBGW와 AI 서버 간 통신, VBGW와 PBX 간 통신 양단에 모두 상호 인증형 보안(mTLS) 인증서 파이프라인 삽입 
  - 게이트웨이 내 메모리 상에 존재하는 인증서 정보가 탈취당하지 않도록 설정 관리 체계(HashiCorp Vault 등) 연동 고려.

### 4.2 음성 데이터 암호화 보존 (Storage Encryption)
- **현재 상태**: 일반 경로에 .wav 파일 평문 기록.
- **최적화 방안**: 
  - 보관 정책(Quota)뿐만이 아닌, 저장하는 순간 AES-256 규격으로 오디오 파일을 암호화 저장.
  - 접근 권한 제어를 통해 악성 콜 녹취본 접근/유출 차단 체계 고도화.

---

> [!TIP]
> **Summary & Next Action**:
> 위 로드맵 중 단기간 내 가장 큰 임팩트를 제공할 수 있는 파트는 **[Phase 1] 레이턴시 최소화(Zero-Copy)**와 **[Phase 3] 그라파나 가시성 확보**입니다. 실 운영 투입 직후 VBGW의 안정성과 품질을 투명하게 입증해 낼 수 있기 때문입니다. 비즈니스 우선순위에 따라 Phase별 Task 범위를 조율할 수 있습니다.
