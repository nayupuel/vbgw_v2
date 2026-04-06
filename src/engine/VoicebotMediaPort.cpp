#include "VoicebotMediaPort.h"

#include "../ai/SileroVad.h"
#include "../ai/VoicebotAiClient.h"
#include "../utils/AppConfig.h"
#include "../utils/RingBuffer.h"

#include <spdlog/spdlog.h>

#include <cstring>

VoicebotMediaPort::VoicebotMediaPort()
    : pj::AudioMediaPort(),
      // [M-7 Fix] TTS 버퍼 크기를 환경변수(TTS_BUFFER_SECS)에서 읽어 동적 설정
      // 16kHz × 2bytes(16bit) × N초 = 32000 × N bytes
      tts_buffer_(std::make_unique<RingBuffer>(static_cast<size_t>(32000) *
                                               AppConfig::instance().tts_buffer_secs)),
      vad_(std::make_unique<SileroVad>())
{
    // AI 모델(STT/TTS)을 위해 16kHz, 1채널 16비트 PCM (20ms) 코덱 포맷 설정
    pj::MediaFormatAudio fmt;
    fmt.type = PJMEDIA_TYPE_AUDIO;
    fmt.clockRate = 16000;
    fmt.channelCount = 1;
    fmt.bitsPerSample = 16;
    fmt.frameTimeUsec = 20000;  // 20 milliseconds

    createPort("VoicebotMediaPort", fmt);
    spdlog::info("[MediaPort] Initialized: 16kHz PCM, {}s TTS buffer ({} bytes)",
                 AppConfig::instance().tts_buffer_secs,
                 static_cast<size_t>(32000) * AppConfig::instance().tts_buffer_secs);
}

// unique_ptr이 완전한 타입(RingBuffer, SileroVad)을 필요로 하기 때문에 .cpp에서 정의
VoicebotMediaPort::~VoicebotMediaPort() {}

void VoicebotMediaPort::setAiClient(std::shared_ptr<VoicebotAiClient> client)
{
    std::lock_guard<std::mutex> lock(client_mutex_);
    ai_client_ = client;
}

void VoicebotMediaPort::setVadSpeechStartCallback(std::function<void()> cb)
{
    on_vad_speech_start_ = std::move(cb);
}

void VoicebotMediaPort::onFrameReceived(pj::MediaFrame& frame)
{
    std::shared_ptr<VoicebotAiClient> safe_client;
    {
        std::lock_guard<std::mutex> lock(client_mutex_);
        safe_client = ai_client_;
    }

    if (frame.type == PJMEDIA_FRAME_TYPE_AUDIO && frame.size > 0 && safe_client) {
        // [Phase2-H6 Fix] 홀수 바이트 가드 — int16_t 경계 정렬 보장
        const size_t safe_size = frame.size & ~static_cast<size_t>(1);
        if (safe_size == 0)
            return;
        const int16_t* pcm16 = reinterpret_cast<const int16_t*>(frame.buf.data());
        size_t samples = safe_size / 2;

        bool is_speaking = vad_->isSpeaking(pcm16, samples);
        if (is_speaking && !last_vad_state_ && on_vad_speech_start_) {
            on_vad_speech_start_();
        }
        last_vad_state_ = is_speaking;

        const uint8_t* raw = reinterpret_cast<const uint8_t*>(frame.buf.data());
        safe_client->sendAudio(raw, safe_size, is_speaking);
    }
}

void VoicebotMediaPort::onFrameRequested(pj::MediaFrame& frame)
{
    frame.type = PJMEDIA_FRAME_TYPE_AUDIO;
    if (frame.buf.size() > 0) {
        size_t read_bytes =
            tts_buffer_->read(reinterpret_cast<uint8_t*>(frame.buf.data()), frame.buf.size());
        // 데이터가 부족하면 나머지를 0(Silence)으로 묵음 처리하여 끊김 잡음(Pop) 방지
        if (read_bytes < frame.buf.size()) {
            std::memset(reinterpret_cast<uint8_t*>(frame.buf.data()) + read_bytes, 0,
                        frame.buf.size() - read_bytes);
        }
        frame.size = frame.buf.size();
    } else {
        frame.size = 0;
    }
}

void VoicebotMediaPort::writeTtsAudio(const uint8_t* data, size_t len)
{
    tts_buffer_->write(data, len);
}

void VoicebotMediaPort::clearTtsAudio()
{
    tts_buffer_->clear();
    spdlog::debug("[MediaPort] TTS buffer cleared (Barge-in).");
}

void VoicebotMediaPort::resetVad()
{
    if (vad_) {
        vad_->resetState();
        last_vad_state_ = false;
        spdlog::debug("[MediaPort] VAD state reset.");
    }
}
