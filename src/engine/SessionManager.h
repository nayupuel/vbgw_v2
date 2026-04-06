#pragma once
#include "../utils/AppConfig.h"
#include "VoicebotCall.h"

#include <memory>
#include <mutex>
#include <unordered_map>

// 싱글톤 기반의 쓰레드 세이프 콜 라이프사이클 관리자
class SessionManager
{
public:
    static SessionManager& getInstance()
    {
        static SessionManager instance;
        return instance;
    }

    void addCall(int call_id, std::shared_ptr<VoicebotCall> call)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        calls_[call_id] = call;
    }

    // [Phase3-M1 Fix] TOCTOU 방지 — 원자적 확인+추가
    bool tryAddCall(int call_id, std::shared_ptr<VoicebotCall> call)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (calls_.size() >= static_cast<size_t>(max_calls_))
            return false;
        calls_[call_id] = call;
        return true;
    }

    void removeCall(int call_id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        calls_.erase(call_id);
    }

    std::shared_ptr<VoicebotCall> getCall(int call_id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = calls_.find(call_id);
        if (it != calls_.end()) {
            return it->second;
        }
        return nullptr;
    }

    bool canAcceptCall()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return calls_.size() < static_cast<size_t>(max_calls_);
    }

    size_t getActiveCallCount()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return calls_.size();
    }

    // 데몬 종료 시 모든 활성 통화 일괄 종료 (Deadlock 방지 위해 복사 후 순회)
    void hangupAllCalls()
    {
        std::vector<std::shared_ptr<VoicebotCall>> active_calls;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& pair : calls_) {
                active_calls.push_back(pair.second);
            }
        }
        for (auto& call : active_calls) {
            try {
                pj::CallOpParam prm;
                prm.statusCode = PJSIP_SC_DECLINE;
                call->hangup(prm);
            } catch (const pj::Error& e) {
                // [T-4 Fix] catch(...) 사일런스 → 명시적 로깅
                spdlog::debug("[SessionManager] hangup suppressed pj::Error: {}", e.info());
            } catch (const std::exception& e) {
                spdlog::debug("[SessionManager] hangup suppressed error: {}", e.what());
            } catch (...) {
                spdlog::debug("[SessionManager] hangup suppressed unknown error");
            }
        }
    }

    // [R-3 Fix] Graceful Shutdown 시 각 콜의 gRPC AI 세션을 명시적으로 종료
    // hangupAllCalls() → onCallState(DISCONNECTED) 체인이 타임아웃 내에
    // 완료되지 않을 수 있으므로, AI 스트림을 직접 정리하여 orphan 방지
    void endAllAiSessions()
    {
        std::vector<std::shared_ptr<VoicebotCall>> active_calls;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& pair : calls_) {
                active_calls.push_back(pair.second);
            }
        }
        spdlog::info("[SessionManager] Ending {} AI sessions...", active_calls.size());
        for (auto& call : active_calls) {
            try {
                call->endAiSession();
            } catch (const std::exception& e) {
                spdlog::debug("[SessionManager] endAiSession suppressed error: {}", e.what());
            } catch (...) {
                spdlog::debug("[SessionManager] endAiSession suppressed unknown error");
            }
        }
    }

private:
    // [H-6 Fix] AppConfig 싱글톤에서 캐싱된 값 사용
    // 기존의 getenv()+stoi() 반복 수행 제거
    SessionManager() : max_calls_(AppConfig::instance().max_concurrent_calls) {}
    ~SessionManager() = default;
    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    const int max_calls_;
    std::unordered_map<int, std::shared_ptr<VoicebotCall>> calls_;
    std::mutex mutex_;
};
