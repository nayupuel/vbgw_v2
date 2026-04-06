#pragma once
#include <cstdint>
#include <memory>
#include <mutex>

// SpeexDSP 래퍼 클래스 — Pimpl 패턴으로 speex 헤더 종속성 캡슐화
// 역할: 16kHz 16bit PCM 프레임에 Denoise(배경음 제거) + AGC(자동 게인 제어) 적용
// 처리 위치: VoicebotMediaPort::onFrameReceived() → SileroVad 추론 전에 호출
//
// 스레드 안전: process()는 내부적으로 dsp_mutex_로 보호됨
// 프레임 크기: 16kHz 20ms = 320 샘플(고정). 다른 크기 입력 시 처리 스킵 + 경고
class SpeexDsp
{
public:
    // sample_rate: 16000 (고정)
    // frame_size : 320 (20ms @ 16kHz, 고정)
    explicit SpeexDsp(int sample_rate = 16000, int frame_size = 320);
    ~SpeexDsp();

    // Denoise + AGC를 in-place로 적용
    // pcm_data : 16bit signed PCM 배열 (read/write)
    // sample_count : 반드시 생성자에서 지정한 frame_size와 동일해야 함
    void process(int16_t* pcm_data, size_t sample_count);

    // Denoise 활성/비활성 (통화 중 런타임 변경 가능)
    void setDenoiseEnabled(bool enabled);
    bool isDenoiseEnabled() const;

    // AGC 활성/비활성
    void setAgcEnabled(bool enabled);
    bool isAgcEnabled() const;

    // AGC 목표 레벨 (0~32768, 기본 16000)
    void setAgcLevel(int level);

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;

    // PJSIP 콜백 스레드와의 동시 접근 방지
    mutable std::mutex dsp_mutex_;

    int frame_size_;
    bool denoise_enabled_;
    bool agc_enabled_;
};
