#include "IvrManager.h"

#include <spdlog/spdlog.h>

IvrManager::IvrManager(const std::string& session_id) : session_id_(session_id) {}

const char* IvrManager::stateName(State s)
{
    switch (s) {
        case State::IDLE:
            return "IDLE";
        case State::MENU:
            return "MENU";
        case State::AI_CHAT:
            return "AI_CHAT";
        case State::TRANSFER:
            return "TRANSFER";
        case State::DISCONNECT:
            return "DISCONNECT";
        default:
            return "UNKNOWN";
    }
}

void IvrManager::activateMenu()
{
    // [P1-Deadlock Fix] 콜백을 뮤텍스 해제 후 실행하는 패턴
    // 콜백이 VoicebotCall::hangup() → PJSIP → SessionManager 체인으로
    // 재진입할 수 있으므로, lock 보유 중 콜백 실행은 deadlock 위험
    std::function<void()> deferred_cb;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (current_state_ == State::IDLE) {
            current_state_ = State::MENU;
            spdlog::info("[IVR] Session={} State: IDLE→MENU (menu activated)", session_id_);
            if (on_repeat_menu_) {
                deferred_cb = on_repeat_menu_;
            }
        }
    }
    if (deferred_cb) {
        deferred_cb();
    }
}

void IvrManager::reset()
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    current_state_ = State::IDLE;
    spdlog::info("[IVR] Session={} State reset to IDLE", session_id_);
}

IvrManager::State IvrManager::getCurrentState() const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    return current_state_;
}

void IvrManager::setDtmfForwardCallback(std::function<void(const std::string&)> cb)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    on_dtmf_forward_ = std::move(cb);
}

void IvrManager::setTransferCallback(std::function<void()> cb)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    on_transfer_ = std::move(cb);
}

void IvrManager::setDisconnectCallback(std::function<void()> cb)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    on_disconnect_ = std::move(cb);
}

void IvrManager::setRepeatMenuCallback(std::function<void()> cb)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    on_repeat_menu_ = std::move(cb);
}

void IvrManager::setEnterAiChatCallback(std::function<void()> cb)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    on_enter_ai_chat_ = std::move(cb);
}

void IvrManager::handleDtmf(const std::string& digit)
{
    if (digit.empty())
        return;

    // [P1-Deadlock Fix] 콜백을 lock 밖에서 실행
    // state_mutex_ 보유 중 콜백 실행 시 PJSIP/SessionManager 뮤텍스와
    // 교차 잠금(Lock Ordering Inversion) 발생 가능
    std::function<void()> deferred_cb;
    std::function<void(const std::string&)> deferred_dtmf_cb;
    std::string forward_digit;

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        spdlog::info("[IVR] Session={} DTMF='{}' State={}", session_id_, digit,
                     stateName(current_state_));

        switch (current_state_) {
            case State::IDLE:
                spdlog::debug("[IVR] Session={} DTMF ignored in IDLE state", session_id_);
                break;

            case State::MENU:
                processMenuDigit(digit, deferred_cb, deferred_dtmf_cb, forward_digit);
                break;

            case State::AI_CHAT:
                processAiChatDigit(digit, deferred_cb, deferred_dtmf_cb, forward_digit);
                break;

            case State::TRANSFER:
                spdlog::debug("[IVR] Session={} DTMF ignored in TRANSFER state", session_id_);
                break;

            case State::DISCONNECT:
                spdlog::debug("[IVR] Session={} DTMF ignored in DISCONNECT state", session_id_);
                break;
        }
    }  // state_mutex_ 해제

    // 콜백을 lock 밖에서 안전하게 실행
    if (deferred_cb) {
        deferred_cb();
    }
    if (deferred_dtmf_cb) {
        deferred_dtmf_cb(forward_digit);
    }
}

// ── 메뉴 상태에서의 DTMF 처리 ──────────────────────────────────────
// [P1-Deadlock Fix] 콜백을 직접 호출하지 않고 out 파라미터로 반환
// lock은 handleDtmf()에서 이미 보유 중
void IvrManager::processMenuDigit(const std::string& digit, std::function<void()>& out_cb,
                                  std::function<void(const std::string&)>& out_dtmf_cb,
                                  std::string& out_digit)
{
    if (digit == "1") {
        current_state_ = State::AI_CHAT;
        spdlog::info("[IVR] Session={} MENU→AI_CHAT", session_id_);
        if (on_enter_ai_chat_)
            out_cb = on_enter_ai_chat_;

    } else if (digit == "0") {
        current_state_ = State::TRANSFER;
        spdlog::info("[IVR] Session={} MENU→TRANSFER (agent requested)", session_id_);
        if (on_transfer_)
            out_cb = on_transfer_;

    } else if (digit == "*") {
        spdlog::info("[IVR] Session={} MENU: repeat requested", session_id_);
        if (on_repeat_menu_)
            out_cb = on_repeat_menu_;

    } else if (digit == "#") {
        current_state_ = State::DISCONNECT;
        spdlog::info("[IVR] Session={} MENU→DISCONNECT", session_id_);
        if (on_disconnect_)
            out_cb = on_disconnect_;

    } else {
        spdlog::info("[IVR] Session={} MENU: forwarding digit '{}' to AI", session_id_, digit);
        if (on_dtmf_forward_) {
            out_dtmf_cb = on_dtmf_forward_;
            out_digit = digit;
        }
    }
}

// ── AI 대화 상태에서의 DTMF 처리 ──────────────────────────────────
// [P1-Deadlock Fix] 콜백을 직접 호출하지 않고 out 파라미터로 반환
void IvrManager::processAiChatDigit(const std::string& digit, std::function<void()>& out_cb,
                                    std::function<void(const std::string&)>& out_dtmf_cb,
                                    std::string& out_digit)
{
    if (digit == "0") {
        current_state_ = State::TRANSFER;
        spdlog::info("[IVR] Session={} AI_CHAT→TRANSFER (agent requested)", session_id_);
        if (on_transfer_)
            out_cb = on_transfer_;

    } else if (digit == "*") {
        current_state_ = State::MENU;
        spdlog::info("[IVR] Session={} AI_CHAT→MENU (back to menu)", session_id_);
        if (on_repeat_menu_)
            out_cb = on_repeat_menu_;

    } else if (digit == "#") {
        current_state_ = State::DISCONNECT;
        spdlog::info("[IVR] Session={} AI_CHAT→DISCONNECT", session_id_);
        if (on_disconnect_)
            out_cb = on_disconnect_;

    } else {
        spdlog::info("[IVR] Session={} AI_CHAT: forwarding digit '{}' to AI", session_id_, digit);
        if (on_dtmf_forward_) {
            out_dtmf_cb = on_dtmf_forward_;
            out_digit = digit;
        }
    }
}
