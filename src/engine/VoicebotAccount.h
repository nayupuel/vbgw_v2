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

    // [Shutdown Fix] PJSIP Account를 명시적으로 정리
    // main() Graceful Shutdown 시 ep.shutdown() 전에 호출
    // 비동기 answer future를 대기하고, PJSIP Account 등록 해제
    void shutdown();

private:
    // [C-3 Fix] detach() 대신 future 보관 — 소멸자에서 완료 보장
    std::mutex futures_mutex_;
    std::vector<std::future<void>> answer_futures_;

    // Outbound makeCall 직렬화 (PJSIP account thread-safety 보호)
    std::mutex outbound_mutex_;

    // shutdown()이 이미 호출되었는지 추적 — 소멸자에서의 이중 정리 방지
    bool shutdown_called_ = false;
};
