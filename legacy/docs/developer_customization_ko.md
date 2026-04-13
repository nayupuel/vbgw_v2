# VBGW 개발자 커스터마이징 가이드 (콜인프라/콜봇 서비스용)

이 문서는 **PJSIP 기본 기능 + 프로젝트 확장 기능**을 바탕으로, 콜인프라 또는 콜봇 서비스 환경에 맞게 VBGW를 커스터마이징하려는 개발자를 위한 가이드입니다.

- 대상: C++/SIP/gRPC 기반 백엔드 개발자, 콜플랫폼 엔지니어
- 범위: 설정 설계, 코드 확장 포인트, API/메트릭 연동, 테스트/운영 반영
- 비범위: 입문 설치/실행 튜토리얼 (초보자는 `docs/user_manual_ko.md` 먼저 권장)

---

## 1. 설계 철학: Config-First + Module-Hook

VBGW는 신규 기능을 “바로 코드 하드코딩”하지 않고, 아래 순서로 확장하는 것을 권장합니다.

1. `AppConfig`에 환경변수 추가
2. `main`/`Endpoint`/`Call`에 매핑
3. 필요 시 Admin API(`/api/v1/*`) 제어면 추가
4. `RuntimeMetrics`와 `/metrics` 관측성 추가
5. `docs`와 테스트 시나리오 업데이트

이 패턴을 지키면 서비스별 분기(고객사 A/B, PBX 벤더별 설정)를 빌드 분기 없이 운영할 수 있습니다.

---

## 2. 현재 코드베이스의 커스터마이징 지형도

### 2.1 SIP 엔드포인트/트랜스포트

- 파일: `src/engine/VoicebotEndpoint.cpp`, `src/engine/VoicebotEndpoint.h`
- 역할:
  1. PJSIP 초기화 (`libCreate/libInit/libStart`)
  2. STUN 서버 주입
  3. UDP/TCP/TLS 멀티 트랜스포트 기동
  4. preferred transport 선택 및 account 바인딩 지원

핵심 포인트:

1. 멀티 리슨 추가는 `startTransport()` 패턴을 재사용
2. 트랜스포트 우선순위 정책은 `choosePreferredTransport()`에서 통합
3. NAT 계열의 endpoint 공통값(STUN 등)은 `init()`에서 설정

### 2.2 계정/콜 정책 매핑 (PJSUA2 AccountConfig)

- 파일: `src/main.cpp`
- 역할:
  1. `AppConfig` → `pj::AccountConfig` 변환
  2. NAT(`natConfig`), 세션타이머/PRACK(`callConfig`) 정책 반영
  3. SRTP 강제화, RTCP Mux/FB 등 미디어 정책 반영

핵심 포인트:

1. 문자열 모드형 설정(`off/optional/mandatory`)은 parse 함수로 enum 변환
2. 보안 강제 로직은 프로파일 기반(`production`)으로 이중 보호
3. 신규 PJSIP 옵션은 이 파일에 “단일 매핑 지점”을 유지

### 2.3 콜 오케스트레이션/고급 콜백

- 파일: `src/engine/VoicebotCall.cpp`, `src/engine/VoicebotCall.h`
- 역할:
  1. 통화 생애주기, 미디어 연결, AI 세션 연결
  2. REFER/Replaces/Redirect 콜백 정책 적용
  3. DTMF 송신(peer/ai), transfer, recording, call bridge
  4. RTP/RTCP/JitterBuffer 통계 snapshot 생성

핵심 포인트:

1. PJSIP API 호출 시 스레드 등록(`pj_thread_register`) 보장
2. 콜백 내부에서는 락 범위를 최소화
3. `RtpStatsSnapshot` 같은 구조체를 통해 HTTP/metrics 계층과 느슨하게 결합

### 2.4 AI 스트리밍 경로

- 파일: `src/ai/VoicebotAiClient.cpp`, `src/engine/VoicebotMediaPort.cpp`
- 역할:
  1. bi-dir gRPC 스트리밍
  2. DTMF 이벤트를 AudioChunk 확장 필드로 전달
  3. Barge-in, 재연결(backoff), queue backpressure 처리
  4. VAD/SpeexDSP 전처리 이후 AI 전송

핵심 포인트:

1. 오디오 프레임과 제어 이벤트(DTMF)를 동일 큐로 다룰 때 우선순위 정책을 서비스별로 조정 가능
2. 영구 오류와 재시도 가능 오류를 구분하는 것이 안정성 핵심
3. STT/TTS 확장 필드가 늘어날 경우 proto와 송수신 처리기를 함께 변경

### 2.5 Admin API(Control Plane)

- 파일: `src/api/HttpServer.cpp`, `src/api/HttpServer.h`
- 역할:
  1. health/live/ready/metrics
  2. outbound call 및 in-call control API
  3. 인증, 요청 제한(rate limit), 유효성 검증, 감사 로그

현재 확장된 제어 API:

1. `POST /api/v1/calls`
2. `POST /api/v1/calls/{id}/dtmf`
3. `POST /api/v1/calls/{id}/transfer`
4. `POST /api/v1/calls/{id}/record/start`
5. `POST /api/v1/calls/{id}/record/stop`
6. `GET /api/v1/calls/{id}/stats`
7. `POST /api/v1/calls/bridge`
8. `POST /api/v1/calls/unbridge`

핵심 포인트:

1. 신규 제어 API는 인증/권한/유효성 검증을 반드시 재사용
2. 상태 변경 API는 모두 감사 로그(Audit log) 남기기
3. 고부하 상황에서 health probe와 제어 API가 간섭하지 않도록 worker 설계 유지

### 2.6 메트릭/관측성

- 파일: `src/utils/RuntimeMetrics.h`, `src/api/HttpServer.cpp`(`handleMetrics`)
- 역할:
  1. lock-free atomic 카운터 registry
  2. Prometheus 포맷 노출
  3. health JSON 요약

핵심 포인트:

1. 신규 기능 추가 시 최소 1개 성공지표 + 1개 실패지표를 같이 추가
2. 카운터/게이지 의미를 문서에 명시 (단위 포함)
3. 운영 알람 룰과 함께 설계

---

## 3. 서비스 유형별 커스터마이징 전략

### 3.1 콜인프라(통신사/SBC 중심) 프로파일

권장:

1. TLS + SRTP mandatory
2. SIP outbound/NAT rewrite 켜기
3. PRACK optional~mandatory (상호운용성 테스트 기준)
4. Session timer required/always
5. RTP keepalive, RTCP XR 활성

검토 포인트:

1. SBC가 RTP/RTCP mux를 지원하는지
2. TURN이 실제로 필요한 토폴로지인지
3. REFER/Replaces 정책 (무조건 수용 금지)

### 3.2 콜봇 서비스(대화 품질 중심) 프로파일

권장:

1. VAD/SpeexDSP 튜닝(잡음 환경)
2. AI stream deadline/reconnect 정책 강화
3. barge-in 민감도 및 TTS buffer 전략 최적화
4. in-call API(dtmf/transfer/recording/stats) 적극 활용

검토 포인트:

1. AI 응답지연 시 버퍼 파라미터
2. 녹취 정책(파일명 규칙, 저장소 정책, 보관기간)
3. 관측 지표와 품질 KPI 연계

---

## 4. 설정(AppConfig) 확장 규칙

파일: `src/utils/AppConfig.h`

새 환경변수 추가 시 체크리스트:

1. 멤버 변수 추가
2. `readBool/readInt/readStr` 로드 로직 추가
3. 범위 보정(clamp/swap) 로직 추가
4. `validateRuntimeSecurityPolicy()`에 production 제약 추가
5. `.env.example`와 `.env.local` 문서화
6. 관련 문서(`README`, `docs/api_spec.md`, `docs/testing.md`) 업데이트

권장 네이밍:

1. SIP 계층: `SIP_*`
2. RTP/미디어 계층: `RTP_*`, `JB_*`
3. 보안: `*_TLS_*`, `SRTP_*`, `ADMIN_*`
4. 기능 스위치: `*_ENABLE`
5. 모드 문자열: `*_MODE`

---

## 5. 기능 추가 절차 (실무 템플릿)

예: “통화중 코덱 강제 재협상 API”를 추가한다면

1. `AppConfig`:
   1. 기능 스위치/기본값 추가
2. `VoicebotCall`:
   1. 실제 PJSUA2 동작 함수 추가 (`reinvite/update` 등)
3. `HttpServer`:
   1. 라우팅 추가
   2. body 파싱/유효성 검사
   3. 인증/감사 로그
4. `RuntimeMetrics`:
   1. 성공/실패 카운터 추가
5. 문서:
   1. API 스펙/테스트 가이드/운영 가이드 반영
6. 검증:
   1. 빌드
   2. 단위테스트
   3. e2e 스크립트 시나리오 추가

---

## 6. 콜백/스레드 안전 원칙 (중요)

1. PJSIP 콜백은 내부 워커 스레드에서 호출될 수 있음
2. gRPC read/write 스레드와 콜 스레드가 교차
3. HTTP worker 스레드에서 콜 제어 API가 들어옴

필수 원칙:

1. PJSIP API 진입 전 thread registration 보장
2. 장시간 블로킹 로직을 콜백에서 직접 수행하지 않기
3. shared state는 mutex/atomic으로 보호
4. 상호 참조 객체(`Call`-`AI client`-`MediaPort`)는 수명관리를 명확히

---

## 7. API/보안 커스터마이징 가이드

### 7.1 인증

현재: `X-Admin-Key` 고정키 방식

확장 아이디어:

1. mTLS 기반 내부망 인증
2. JWT 기반 role 분리 (`ops`, `qa`, `admin`)
3. endpoint별 권한 스코프

### 7.2 유효성 검증

필수:

1. SIP URI 인젝션 방지(CRLF/특수문자)
2. call_id 범위/존재성 검증
3. payload/header 사이즈 제한
4. rate-limit + Retry-After 제공

### 7.3 감사 로그

필수:

1. 누가(키/호스트) 무엇을(엔드포인트/파라미터) 호출했는지
2. 성공/실패 결과 및 이유

---

## 8. 메트릭 설계 가이드

신규 기능 추가 시 아래 3종을 최소로 권장합니다.

1. 요청 카운터(total)
2. 실패 카운터(error_total)
3. 현재 상태 게이지(active/queue/latency)

권장 네이밍 예:

1. `vbgw_feature_requests_total`
2. `vbgw_feature_failures_total`
3. `vbgw_feature_active`

RTCP 품질 계열은 단위 명시 필수:

1. `*_usec_*`
2. `*_ms_*`

---

## 9. 테스트 전략

### 9.1 기본 검증

```bash
cmake --build build
ctest --test-dir build --output-on-failure
bash tests/test_validate_prod_env.sh
```

### 9.2 시나리오 검증

1. 로컬 통합: `./test.sh mock null`
2. outbound e2e: `./scripts/e2e_outbound_null_audio.sh config/.env.local`
3. control-plane soak: `./scripts/soak_outbound_api.sh ...`

### 9.3 신규 기능 회귀 체크리스트

1. 기존 `/live`, `/ready`, `/health` 영향 없는지
2. `/metrics` 포맷 깨지지 않는지
3. max call 상황(용량 한계)에서 에러 코드 일관성
4. graceful shutdown 시 orphan stream/recording 없는지

---

## 10. 운영 반영 체크리스트

1. production profile 정책 통과
2. TLS/SRTP/grpc TLS 인증서 유효성
3. rate-limit 값과 트래픽 패턴 정합
4. 디스크(녹취) 용량/보관 정책
5. 알람 룰 설정 (`stream_errors`, `dropped_frames`, `rtp_lost`)

---

## 11. 문서 간 역할 분리

1. `docs/user_manual_ko.md`: 비개발자/초보자 실행 가이드
2. `docs/testing.md`: 테스트 시나리오 중심
3. `docs/api_spec.md`: 메시지/제어 API 규약
4. `docs/developer_customization_ko.md`(본 문서): 개발자 커스터마이징 기준

---

## 12. 권장 개발 워크플로우 (팀 규칙 제안)

1. 이슈 등록 시 “설정키, 코드포인트, API, 메트릭, 테스트” 5항목 템플릿 사용
2. PR 리뷰 시 아래를 체크
   1. 하드코딩 금지
   2. production guardrail 유지
   3. API 인증/검증 누락 없음
   4. 메트릭 추가 여부
   5. 문서 갱신 여부

이 규칙을 지키면 기능이 많아져도 운영 안정성을 유지하면서 빠르게 확장할 수 있습니다.
