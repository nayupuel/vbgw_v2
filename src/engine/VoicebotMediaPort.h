#pragma once
#include <pjsua2.hpp>

#include <functional>
#include <memory>
#include <mutex>

class VoicebotAiClient;
class RingBuffer;
class SileroVad;

class VoicebotMediaPort : public pj::AudioMediaPort
{
public:
    VoicebotMediaPort();
    void setAiClient(std::shared_ptr<VoicebotAiClient> client);
    virtual ~VoicebotMediaPort();  // unique_ptr 소멸자는 cpp에서 완전 타입 필요

    // 수신 (Rx): PBX에서 보낸 사용자(고객) 음성이 들어오는 곳 -> STT 스트리밍용
    virtual void onFrameReceived(pj::MediaFrame& frame) override;

    // 송신 (Tx): PBX로 보낼 봇(TTS) 음성을 요청받는 곳 -> TTS 재생 버퍼
    virtual void onFrameRequested(pj::MediaFrame& frame) override;

    // AI 엔진에서 내려온 TTS 오디오를 재생 버퍼에 쓰기
    void writeTtsAudio(const uint8_t* data, size_t len);

    // VAD 감지 등 AI 서버의 말끊기 응답 시 재생 버퍼 잔량 강제 폐기(Flush)
    void clearTtsAudio();

    // 강제 VAD 초기화 (Barge-in 시 상태 소거용)
    void resetVad();

    // VAD speech-start 이벤트 콜백 (false->true edge)
    void setVadSpeechStartCallback(std::function<void()> cb);

private:
    std::unique_ptr<RingBuffer> tts_buffer_;  // RAII
    std::shared_ptr<VoicebotAiClient> ai_client_;
    std::unique_ptr<SileroVad> vad_;  // RAII

    std::function<void()> on_vad_speech_start_;
    bool last_vad_state_ = false;

    // Thread safety for ai_client_
    std::mutex client_mutex_;
};
