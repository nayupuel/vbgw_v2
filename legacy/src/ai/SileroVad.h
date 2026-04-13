#pragma once
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Pimpl Idiom을 사용하여 ONNX 헤더 종속성을 캡슐화합니다.
class SileroVad
{
public:
    // 모델 경로: SILERO_VAD_MODEL_PATH 환경변수 → 기본 "models/silero_vad.onnx"
    SileroVad();
    ~SileroVad();

    // 16kHz PCM(int16) 배열을 입력받아 화자 발화 점수(Probability)에 기반한 VAD 판별
    // threshold: 기본 0.5 (Silero 권장치)
    // [M-5 Fix] vad_mutex_로 pcm_buffer_ / last_speaking_state_ 보호
    bool isSpeaking(const std::vector<int16_t>& pcm, float threshold = 0.5f);

    // [P-1 Fix] 포인터 오버로드 — 호출측 벡터 복사 없이 직접 전달
    bool isSpeaking(const int16_t* data, size_t count, float threshold = 0.5f);

    // 새로운 통화 시작 시 LSTM의 내부 상태(State)를 초기화
    void resetState();

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;

    // [M-5 Fix] VoicebotMediaPort 스레드와 PJSIP 콜백 스레드의 동시 호출 방지
    mutable std::mutex vad_mutex_;
    std::vector<int16_t> pcm_buffer_;
    // [P-1 Fix] head offset — erase(begin, begin+512) O(N) shift 대신 주기적 compact
    size_t pcm_head_ = 0;
    bool last_speaking_state_ = false;

    // vad_mutex_ 보유 가정 — 두 public 오버로드의 공통 구현
    bool isSpeakingImpl(const int16_t* data, size_t count, float threshold);
};
