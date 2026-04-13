# Troubleshooting Guide

Voicebot Gateway(VBGW) 운영 중 발생 가능한 문제와 해결 방법입니다.

## 1. 빌드 및 설치 관련 (Build & Installation)

### 1.1 ONNX Runtime 라이브러리 미인식
- **현상**: `find_package` 또는 `find_library`에서 `onnxruntime`을 찾지 못함.
- **해결**: 최신 빌드 시스템은 `/opt/homebrew`나 `/usr/local`과 같은 표준 경로를 자동 탐색합니다. 직접 컴파일했거나 특수한 경로에 설치한 경우에는 CMake 실행 시 힌트를 제공하십시오:
```bash
  cmake -DCMAKE_INCLUDE_PATH=/custom/onnx/include -DCMAKE_LIBRARY_PATH=/custom/onnx/lib ..
```

### 1.2 PJSIP(PJPROJECT) 의존성 오류
- **현상**: 컴파일 시 `pjsua.h` 헤더를 찾을 수 없음.
- **해결**: `brew install pjproject`가 정상적으로 완료되었는지 확인하고, `pkg-config --cflags libpjproject` 명령어로 경로가 출력되는지 점검하십시오.

## 2. 런타임 오류 (Runtime Issues)

### 2.1 VAD 모델 로드 실패
- **현상**: 실행 시 `Silero VAD 모델을 찾을 수 없습니다` 에러 발생.
- **해결**: `models/silero_vad.onnx` 파일이 프로젝트 루트 하위에 존재하는지 확인하십시오. 실행 시 작업 디렉토리(CWD)가 프로젝트 루트여야 합니다.

### 2.2 gRPC 스트림 연결 실패
- **현상**: `[gRPC] Stream failed: Deadline Exceeded` 또는 연결 거부.
- **해결**: AI 엔진(에뮬레이터 등)이 `localhost:50051`에서 정상 동작 중인지 확인하십시오. 방화벽 설정에 의해 포트가 차단되지 않았는지 점검하십시오.

### 2.3 음성 끊김 또는 지연 (Audio Stuttering/Latency)
- **현상**: 사용자의 음성이 AI에게 느게 전달되거나 지터 발생.
- **해결**: 
    - 네트워크 환경이 RTP 패킷 손실을 유발하지 않는지 확인하십시오.
    - `VoicebotMediaPort`의 `RingBuffer` 사이즈를 조정하여 지연과 안정성 사이의 균형을 맞추십시오.
    - CPU 사용량이 과도할 경우 `SileroVAD`의 `IntraOpNumThreads`를 1로 제한했는지 확인하십시오.

## 3. PBX 연동 관련 (PBX Integration)

### 3.1 등록 실패 (401 Unauthorized)
- **현상**: `PBX Registration Mode Enabled` 로그 이후 `Error creating account` 발생.
- **해결**: 환경 변수 `PBX_USERNAME`과 `PBX_PASSWORD`가 PBX 설정과 일치하는지 확인하십시오.

### 3.2 수신 호 무응답
- **현상**: INVITE 신호가 오지만 게이트웨이가 응답하지 않음.
- **해결**: 게이트웨이가 SIP 포트(기본 5060)에서 Listen 중인지 `netstat -an`으로 확인하고, PBX 시스템에서 게이트웨이의 IP 정보가 올바르게 등록되었는지 점검하십시오.

### 3.3 수신 호 즉시 거절 (486 Busy Here)
- **현상**: INVITE 신호가 오자마자 게이트웨이가 `486 Busy Here`로 응답.
- **해결**: 허용된 최대 동시 통화 수(`MAX_CONCURRENT_CALLS`)를 초과한 상태입니다. 시스템 자원에 여유가 있다면 환경 변수 `MAX_CONCURRENT_CALLS` 값을 상향 조정 후 재시작하세요.

## 4. 로그 및 모니터링 (Logging & Observability)

### 4.1 로그 레벨 조정
- **현상**: 게이트웨이의 내부 동작(예: SIP 패킷 덤프, 상세 VAD 작동 내역)을 디버깅하고 싶은 경우.
- **해결**: 환경 변수 `LOG_LEVEL=debug` 또는 `LOG_LEVEL=trace` 설정 후 재실행하십시오. (지원 레벨: trace, debug, info, warn, error, critical)

### 4.2 `/api/v1/calls`가 401/403을 반환함
- **현상**: Outbound API 호출 시 Unauthorized/Forbidden 응답.
- **해결**: `X-Admin-Key` 헤더가 설정되어 있는지, 그리고 `ADMIN_API_KEY` 환경변수와 정확히 일치하는지 확인하십시오.

### 4.3 `/api/v1/calls`가 429를 반환함
- **현상**: Outbound API 호출이 Too Many Requests로 거절됨.
- **해결**:
  1. `MAX_CONCURRENT_CALLS` 한계 초과 여부 확인 (`vbgw_active_calls` 관측).
  2. `ADMIN_API_RATE_LIMIT_RPS` / `ADMIN_API_RATE_LIMIT_BURST` 설정 확인.
  3. 응답 헤더 `Retry-After`를 준수해 재시도 백오프를 적용하십시오.

### 4.4 운영 환경 보안 설정 점검
- **현상**: 배포 후 기동 실패 또는 보안 정책 경고.
- **해결**: 배포 전에 아래 명령으로 필수 보안 설정을 검증하십시오.
```bash
VALIDATE_PROFILE=production REQUIRE_PRODUCTION_PROFILE=1 ./scripts/validate_prod_env.sh .env
```

### 4.5 E2E 스크립트가 `PJMEDIA_EAUD_INIT`으로 SKIP/실패
- **현상**: `scripts/e2e_outbound_null_audio.sh` 실행 시 오디오 서브시스템 초기화 실패.
- **원인**: 실행 환경의 미디어/오디오 백엔드 제약(샌드박스, 드라이버, 권한).
- **해결**:
  1. `PJSIP_NULL_AUDIO=1` 설정 확인.
  2. `pjsua --null-audio --version` 동작 여부 확인.
  3. CI에서는 `pjproject` 설치 여부 및 `pjsua` PATH 확인.
  4. 미디어 강제 검증이 필요하면 `REQUIRE_MEDIA=1` 옵션으로 실행.

### 4.6 `/ready`가 503을 반환함
- **현상**: 프로세스는 살아 있으나 readiness 실패.
- **해결**:
  1. `/health`의 `sip.registered`, `grpc.healthy` 필드를 확인.
  2. PBX 모드라면 SIP 등록 상태 복구 후 재확인.
  3. gRPC 스트림 오류가 누적될 경우 AI 엔진 연결/TLS 인증서/네트워크를 점검.
