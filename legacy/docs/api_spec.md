# gRPC API Specification

Voicebot Gateway (VBGW)와 AI Engine(STT/TTS) 간의 통신을 위한 gRPC API 규격서입니다.

## 1. 서비스 개요 (Service Overview)

VBGW는 AI 엔진과 **Bi-directional Streaming** 방식을 사용하여 음성 데이터와 텍스트(STT/TTS) 이벤트를 실시간으로 주고받습니다.

- **Proto File**: `protos/voicebot.proto`
- **Service Name**: `voicebot.ai.VoicebotAiService`
- **RPC Method**: `StreamSession`

## 2. 메시지 규격 (Message Specifications)

### 2.1 AudioChunk (Gateway -> AI Engine)
콜봇 게이트웨이에서 AI 엔진으로 전달되는 오디오 데이터 및 상태 정보입니다.

| 필드명 | 타입 | 설명 |
| :--- | :--- | :--- |
| `session_id` | `string` | PJSIP에서 생성된 고유 Call ID |
| `audio_data` | `bytes` | 16kHz, 16bit, Mono PCM 바이너리 (최소 20ms 단위) |
| `is_speaking` | `bool` | 게이트웨이 내부 Silero VAD에 의한 음성 감지 여부 |
| `dtmf_digit` | `string` | DTMF 단일 문자 (`0-9`, `*`, `#`, `A-D`). 비어있으면 일반 오디오 프레임 |

### 2.2 AiResponse (AI Engine -> Gateway)
AI 엔진에서 게이트웨이로 전달되는 처리 결과 및 음성 데이터입니다.

| 필드명 | 타입 | 설명 |
| :--- | :--- | :--- |
| `type` | `enum` | 응답 유형 (`STT_RESULT`, `TTS_AUDIO`, `END_OF_TURN`) |
| `text_content` | `string` | STT 인식 결과 텍스트 또는 TTS 원문 텍스트 |
| `audio_data` | `bytes` | Gateway가 재생해야 할 16kHz PCM 바이너리 |
| `clear_buffer` | `bool` | `true`일 경우 게이트웨이의 TTS 재생 버퍼를 즉시 비움 (Barge-in 처리) |

## 3. 통신 흐름 (Communication Flow)

1.  **세션 시작**: VBGW가 `StreamSession` 호출을 시작합니다.
2.  **데이터 송신**: VBGW는 RTP로 수신된 고객 음성을 PCM으로 변환하여 실시간으로 `AudioChunk`를 전송합니다.
3.  **결과 수신**: AI 엔진은 음성을 분석하여 `STT_RESULT`를 보내고, 답변이 준비되면 `TTS_AUDIO`를 스트리밍합니다.
4.  **Barge-in**: 사용자가 AI 답변 중 말을 시작하면, AI 엔진은 `END_OF_TURN`과 함께 `clear_buffer=true`를 보내어 게이트웨이의 재생을 중단시킵니다.
5.  **세션 종료**: 통화가 종료되면 VBGW가 `WritesDone`을 호출하고 스트림을 닫습니다.

## 5. Admin Control API (HTTP)

운영 제어용 Admin API(`X-Admin-Key` 필요)는 아래 엔드포인트를 제공합니다.

| Method | Path | 설명 |
| :--- | :--- | :--- |
| `POST` | `/api/v1/calls` | Outbound 콜 생성 (`target_uri`) |
| `POST` | `/api/v1/calls/{call_id}/dtmf` | 통화 중 DTMF 송신 (`digits`, `target=peer|ai|both`) |
| `POST` | `/api/v1/calls/{call_id}/transfer` | 블라인드 전환 (REFER) 요청 (`target_uri`) |
| `POST` | `/api/v1/calls/{call_id}/record/start` | 통화 녹취 시작 (`file_path` 선택) |
| `POST` | `/api/v1/calls/{call_id}/record/stop` | 통화 녹취 중지 |
| `GET` | `/api/v1/calls/{call_id}/stats` | RTP/RTCP/JitterBuffer 통계 조회 |
| `POST` | `/api/v1/calls/bridge` | 2개 통화를 컨퍼런스 브리지로 연결 (`call_a`, `call_b`) |
| `POST` | `/api/v1/calls/unbridge` | 브리지 해제 (`call_a`, `call_b`) |

## 4. 에러 및 예외 처리 (Error Handling)

네트워크 단절이나 AI 엔진 장애 발생 시, gRPC `Status` 코드를 기반으로 게이트웨이가 다음과 같이 즉각적인 통화 복구 및 종료 액션을 수행합니다.

| Status.Code | 에러 설명 | 게이트웨이(VBGW) 동작 시나리오 |
| :--- | :--- | :--- |
| `OK (0)` | 정상 종료 | 정상적으로 통화 절차를 마무리합니다. |
| `UNAVAILABLE (14)` | 서비스 연결 불가 | 지수 백오프 기반(Exponential Backoff)으로 재연결을 즉각 5회 시도합니다. |
| `DEADLINE_EXCEEDED (4)` | 응답 타임아웃 | 스트림을 종료 후 재연결 파이프라인으로 전환합니다. |
| `UNKNOWN (2)`, `INTERNAL (13)` | 서버 측 심각한 오류 | 복구 불능 상태로 간주하여 현재 SIP 호를 `503 Service Unavailable` 혹은 `BYE`로 강제 종료시킵니다. |
