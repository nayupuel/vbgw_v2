#pragma once
#include <pjsua2.hpp>

#include <future>
#include <mutex>
#include <string>
#include <vector>

class VoicebotAccount : public pj::Account
{
public:
    VoicebotAccount();
    ~VoicebotAccount() override;

    virtual void onRegState(pj::OnRegStateParam& prm) override;
    virtual void onIncomingCall(pj::OnIncomingCallParam& iprm) override;

    // HTTP Admin API 등 외부 제어 경로에서 발신 콜 시작
    bool makeOutboundCall(const std::string& target_uri, int* out_call_id = nullptr,
                          std::string* error_message = nullptr);

private:
    // [C-3 Fix] detach() 대신 future 보관 — 소멸자에서 완료 보장
    std::mutex futures_mutex_;
    std::vector<std::future<void>> answer_futures_;

    // Outbound makeCall 직렬화 (PJSIP account thread-safety 보호)
    std::mutex outbound_mutex_;
};
