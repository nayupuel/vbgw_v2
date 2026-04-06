# VBGW SLO & On-call Runbook

## 1. 서비스 목표 (SLO)

### 1.1 API/제어면 SLO
- `/health` 성공률: **99.95% / 30일**
- `/api/v1/calls` 202 비율(정상 인증 기준): **99.9% / 30일**
- `/api/v1/calls` P95 응답지연: **300ms 이하**

### 1.2 미디어/스트리밍 SLO
- gRPC 스트림 정상 유지율: **99.9% / 30일**
- gRPC 재연결 성공률(재시도 허용 범위 내): **99% 이상**
- 오디오 큐 드롭 프레임 비율: **0.1% 이하**

## 2. 핵심 지표 (/metrics)

| 지표 | 의미 | 운영 기준 |
|---|---|---|
| `vbgw_active_calls` | 현재 활성 통화 수 | 용량 한계 대비 80% 이상이면 경고 |
| `vbgw_sip_registered` | PBX 등록 상태 | 0 지속 시 즉시 장애 대응 |
| `vbgw_grpc_active_sessions` | 현재 gRPC 세션 수 | active_calls와 큰 괴리 여부 점검 |
| `vbgw_grpc_queued_frames` | gRPC 송신 대기 프레임 | 지속 증가 시 AI 지연/병목 의심 |
| `vbgw_grpc_dropped_frames_total` | 드롭 누적 | 급증 시 품질 저하 대응 |
| `vbgw_grpc_reconnect_attempts_total` | 재연결 누적 | 급증 시 네트워크/AI 장애 점검 |
| `vbgw_grpc_stream_errors_total` | 스트림 오류 누적 | 증가 추세면 우선 분석 대상 |
| `vbgw_vad_speech_events_total` | 발화 시작 이벤트 누적 | 트래픽 상관 확인 |
| `vbgw_barge_in_events_total` | barge-in 이벤트 누적 | 대화 모델/UX 품질 상관 확인 |
| `vbgw_admin_api_outbound_requests_total` | Outbound API 총 요청 | 급증 시 트래픽/공격 여부 확인 |
| `vbgw_admin_api_outbound_rejected_auth_total` | 인증 실패 누적 | 키 불일치/비인가 접근 탐지 |
| `vbgw_admin_api_outbound_rejected_rate_limited_total` | rate-limit 누적 | API 과부하 또는 공격 신호 |
| `vbgw_admin_api_outbound_failed_total` | 내부 실패 누적 | SIP/gRPC/용량 이슈와 상관 분석 |

## 3. 알람 권장 규칙

1. **Critical**
- `vbgw_sip_registered == 0` 상태가 2분 이상 지속
- `vbgw_grpc_stream_errors_total` 5분 증가량이 임계치 초과

2. **Warning**
- `vbgw_grpc_queued_frames`가 5분 이상 증가 추세
- `vbgw_grpc_dropped_frames_total` 증가율이 기준 이상
- `vbgw_active_calls`가 `MAX_CONCURRENT_CALLS`의 80% 이상

## 4. 장애 대응 절차

1. **SIP 등록 이상**
- `/health` 확인: sip.registered, sip.last_status_code 확인
- PBX 연결/인증 정보(`PBX_*`) 확인
- 필요 시 인스턴스 재기동 전 PBX 상태 먼저 점검

2. **gRPC 스트림 장애**
- `/health`의 grpc fields 확인 (`healthy`, `stream_errors_total`, `reconnect_attempts_total`)
- AI 엔진 endpoint/네트워크/TLS 인증서 유효성 점검
- 재연결 한도 초과 시 해당 통화는 `503` 종료 정책 적용 여부 확인

3. **오디오 품질 저하**
- `queued_frames`, `dropped_frames_total` 증가 여부 확인
- AI 응답 지연, CPU 부하, 네트워크 RTT 및 패킷 손실 확인
- 필요 시 `TTS_BUFFER_SECS` 튜닝 및 AI 처리량 증설

4. **관리 API 이상**
- 인증 실패 비율(401/403) 급증 시 배포된 키 불일치 점검
- 429 증가 시 동시호 한계/큐 전략 재조정

## 5. 배포 전 체크리스트

1. `./scripts/validate_prod_env.sh .env` 통과
  - 권장 게이트: `VALIDATE_PROFILE=production REQUIRE_PRODUCTION_PROFILE=1 ./scripts/validate_prod_env.sh .env`
2. `/health`, `/metrics` 응답 확인
  - 오케스트레이터 연계 시 `/live`(liveness), `/ready`(readiness) 상태코드 확인
3. `scripts/soak_outbound_api.sh`로 제어면 부하 확인
4. `scripts/e2e_outbound_null_audio.sh config/.env.local`로 Outbound E2E 확인
5. `ctest --test-dir build --output-on-failure` 통과
