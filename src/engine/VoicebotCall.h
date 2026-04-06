#pragma once
#include <pjsua2.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>

class VoicebotAiClient;
class VoicebotMediaPort;

// [M-1 Fix] enable_shared_from_this 상속 추가
// 콜백 Lambda에서 this 대신 weak_from_this()를 사용하여 dangling pointer 방지
class VoicebotCall : public pj::Call, public std::enable_shared_from_this<VoicebotCall>
{
public:
    VoicebotCall(pj::Account& acc, int call_id = PJSUA_INVALID_ID);
    ~VoicebotCall();

    virtual void onCallState(pj::OnCallStateParam& prm) override;
    virtual void onCallMediaState(pj::OnCallMediaStateParam& prm) override;
    virtual void onDtmfDigit(pj::OnDtmfDigitParam& prm) override;  // [E-3] IVR 레이어

    // [R-3 Fix] Graceful Shutdown 시 AI 세션 명시적 종료
    void endAiSession();

private:
    std::unique_ptr<VoicebotMediaPort> media_port_;
    std::shared_ptr<VoicebotAiClient> ai_client_;

    // [H-1 Fix] ai_client_ 초기화 이중 진입 방지
    std::mutex ai_init_mutex_;

    // [M-9 Fix] UUID 기반 세션 ID — 분산 환경에서 로그 추적 가능
    std::string session_id_;

    // [E-1] CDR (Call Detail Record) 생성을 위한 생애주기 메트릭
    std::chrono::system_clock::time_point start_time_;
    std::atomic<int> vad_trigger_count_{0};
    std::atomic<int> bargein_count_{0};

    void dumpCdr(const std::string& reason);
};
