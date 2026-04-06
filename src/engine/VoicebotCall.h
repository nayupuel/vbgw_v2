#pragma once
#include <pjsua2.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

class VoicebotAiClient;
class VoicebotMediaPort;
class IvrManager;

// [M-1 Fix] enable_shared_from_this 상속 추가
// 콜백 Lambda에서 this 대신 weak_from_this()를 사용하여 dangling pointer 방지
class VoicebotCall : public pj::Call, public std::enable_shared_from_this<VoicebotCall>
{
public:
    struct RtpStatsSnapshot
    {
        bool valid = false;
        int media_index = -1;
        std::uint64_t rx_packets = 0;
        std::uint64_t tx_packets = 0;
        std::uint64_t rx_lost = 0;
        std::uint64_t rx_discard = 0;
        std::uint64_t rx_reorder = 0;
        std::uint64_t rx_dup = 0;
        std::uint64_t rx_jitter_mean_usec = 0;
        std::uint64_t rtt_mean_usec = 0;
        std::uint64_t jbuf_avg_delay_ms = 0;
        std::uint64_t jbuf_lost = 0;
        std::uint64_t jbuf_discard = 0;
        std::string src_rtp;
        std::string src_rtcp;
    };

    VoicebotCall(pj::Account& acc, int call_id = PJSUA_INVALID_ID);
    ~VoicebotCall();

    virtual void onCallState(pj::OnCallStateParam& prm) override;
    virtual void onCallTsxState(pj::OnCallTsxStateParam& prm) override;
    virtual void onCallMediaState(pj::OnCallMediaStateParam& prm) override;
    virtual void onDtmfDigit(pj::OnDtmfDigitParam& prm) override;  // [E-3] IVR 레이어
    virtual void onCallTransferRequest(pj::OnCallTransferRequestParam& prm) override;
    virtual void onCallTransferStatus(pj::OnCallTransferStatusParam& prm) override;
    virtual void onCallReplaceRequest(pj::OnCallReplaceRequestParam& prm) override;
    virtual void onCallReplaced(pj::OnCallReplacedParam& prm) override;
    virtual pjsip_redirect_op onCallRedirected(pj::OnCallRedirectedParam& prm) override;

    // [R-3 Fix] Graceful Shutdown 시 AI 세션 명시적 종료
    void endAiSession();

    // API 제어 기능
    bool sendDtmfToPeer(const std::string& digits, std::string* error_message = nullptr);
    bool sendDtmfToAi(const std::string& digits, std::string* error_message = nullptr);
    bool transferTo(const std::string& target_uri, std::string* error_message = nullptr);
    bool startRecording(const std::string& file_path, std::string* error_message = nullptr);
    bool stopRecording(std::string* error_message = nullptr);
    bool isRecording() const;
    std::string recordingFilePath() const;
    bool getRtpStatsSnapshot(RtpStatsSnapshot* out, std::string* error_message = nullptr) const;
    bool bridgeWith(const std::shared_ptr<VoicebotCall>& other,
                    std::string* error_message = nullptr);
    bool unbridgeWith(const std::shared_ptr<VoicebotCall>& other,
                      std::string* error_message = nullptr);

private:
    std::unique_ptr<VoicebotMediaPort> media_port_;
    std::shared_ptr<VoicebotAiClient> ai_client_;
    std::unique_ptr<IvrManager> ivr_manager_;
    std::unique_ptr<pj::AudioMediaRecorder> recorder_;

    // [H-1 Fix] ai_client_ 초기화 이중 진입 방지
    std::mutex ai_init_mutex_;
    mutable std::mutex media_mutex_;
    bool recording_active_ = false;
    std::string recording_file_path_;
    int primary_audio_media_index_ = -1;

    // [M-9 Fix] UUID 기반 세션 ID — 분산 환경에서 로그 추적 가능
    std::string session_id_;

    // [E-1] CDR (Call Detail Record) 생성을 위한 생애주기 메트릭
    std::chrono::system_clock::time_point start_time_;
    std::atomic<int> vad_trigger_count_{0};
    std::atomic<int> bargein_count_{0};

    static bool isValidDtmfDigits(const std::string& digits);
    static bool isValidTransferTarget(const std::string& target_uri);
    bool startRecordingLocked(const std::string& file_path, std::string* error_message);
    void dumpCdr(const std::string& reason);
};
