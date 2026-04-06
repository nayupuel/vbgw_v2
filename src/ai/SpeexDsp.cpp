#include "SpeexDsp.h"

#include <spdlog/spdlog.h>

#include <stdexcept>

#include <speex/speex_preprocess.h>

// Pimpl 구현체 — speex 헤더를 .cpp 안으로 격리
struct SpeexDsp::Impl
{
    SpeexPreprocessState* state = nullptr;
    int sample_rate;
    int frame_size;

    Impl(int sr, int fs) : sample_rate(sr), frame_size(fs)
    {
        state = speex_preprocess_state_init(fs, sr);
        if (!state) {
            throw std::runtime_error("[SpeexDsp] speex_preprocess_state_init() failed");
        }
    }

    ~Impl()
    {
        if (state) {
            speex_preprocess_state_destroy(state);
            state = nullptr;
        }
    }

    // 복사/이동 금지 — speex 상태 객체가 포인터이므로
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
};

SpeexDsp::SpeexDsp(int sample_rate, int frame_size)
    : frame_size_(frame_size), denoise_enabled_(true), agc_enabled_(true)
{
    pimpl_ = std::make_unique<Impl>(sample_rate, frame_size);

    // 초기 상태 적용 (생성자 호출 후 set*() 전 기본값)
    spx_int32_t val = 1;
    speex_preprocess_ctl(pimpl_->state, SPEEX_PREPROCESS_SET_DENOISE, &val);
    speex_preprocess_ctl(pimpl_->state, SPEEX_PREPROCESS_SET_AGC, &val);

    spx_int32_t agc_lvl = 16000;
    speex_preprocess_ctl(pimpl_->state, SPEEX_PREPROCESS_SET_AGC_LEVEL, &agc_lvl);

    spdlog::info("[SpeexDsp] Initialized: {}Hz, frame={}samples, denoise=ON, agc=ON", sample_rate,
                 frame_size);
}

SpeexDsp::~SpeexDsp() = default;

void SpeexDsp::process(int16_t* pcm_data, size_t sample_count)
{
    // 프레임 크기 불일치 — SpeexPreprocessState는 고정 크기를 요구
    if (static_cast<int>(sample_count) != frame_size_) {
        spdlog::warn("[SpeexDsp] Frame size mismatch: got {} samples, expected {} — skipping",
                     sample_count, frame_size_);
        return;
    }

    if (!pcm_data) {
        return;
    }

    std::lock_guard<std::mutex> lock(dsp_mutex_);

    if (!pimpl_ || !pimpl_->state) {
        return;
    }

    // speex_preprocess_run(): in-place 처리 (pcm_data 직접 수정)
    // 반환값: 1 = 음성 감지됨, 0 = 음성 없음 (우리는 SileroVad를 사용하므로 무시)
    speex_preprocess_run(pimpl_->state, reinterpret_cast<spx_int16_t*>(pcm_data));
}

void SpeexDsp::setDenoiseEnabled(bool enabled)
{
    std::lock_guard<std::mutex> lock(dsp_mutex_);
    denoise_enabled_ = enabled;
    if (pimpl_ && pimpl_->state) {
        spx_int32_t val = enabled ? 1 : 0;
        speex_preprocess_ctl(pimpl_->state, SPEEX_PREPROCESS_SET_DENOISE, &val);
        spdlog::info("[SpeexDsp] Denoise {}", enabled ? "enabled" : "disabled");
    }
}

bool SpeexDsp::isDenoiseEnabled() const
{
    std::lock_guard<std::mutex> lock(dsp_mutex_);
    return denoise_enabled_;
}

void SpeexDsp::setAgcEnabled(bool enabled)
{
    std::lock_guard<std::mutex> lock(dsp_mutex_);
    agc_enabled_ = enabled;
    if (pimpl_ && pimpl_->state) {
        spx_int32_t val = enabled ? 1 : 0;
        speex_preprocess_ctl(pimpl_->state, SPEEX_PREPROCESS_SET_AGC, &val);
        spdlog::info("[SpeexDsp] AGC {}", enabled ? "enabled" : "disabled");
    }
}

bool SpeexDsp::isAgcEnabled() const
{
    std::lock_guard<std::mutex> lock(dsp_mutex_);
    return agc_enabled_;
}

void SpeexDsp::setAgcLevel(int level)
{
    std::lock_guard<std::mutex> lock(dsp_mutex_);
    if (pimpl_ && pimpl_->state) {
        spx_int32_t val = static_cast<spx_int32_t>(level);
        speex_preprocess_ctl(pimpl_->state, SPEEX_PREPROCESS_SET_AGC_LEVEL, &val);
        spdlog::info("[SpeexDsp] AGC level set to {}", level);
    }
}
