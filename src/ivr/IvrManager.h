#pragma once
#include <functional>
#include <mutex>
#include <string>

// IVR 상태머신 — 통화 중 DTMF 입력을 처리하여 AI 대화/상담원 전환/통화 종료를 제어
//
// 상태 전환 다이어그램:
//   IDLE → MENU (초기화 완료 시)
//   MENU + '1' → AI_CHAT          (AI 대화 모드)
//   MENU + '0' → TRANSFER         (상담원 연결)
//   MENU + '*' → MENU             (메뉴 반복)
//   MENU + '#' → DISCONNECT       (통화 종료)
//   MENU + '2'-'9' → AI 엔진 포워딩
//   AI_CHAT + '0' → TRANSFER
//   AI_CHAT + '*' → MENU          (메뉴로 복귀)
//   AI_CHAT + '#' → DISCONNECT
//   AI_CHAT + '2'-'9' → AI 엔진 포워딩
//   TRANSFER → 모든 입력 무시 (상담원 대기 중)
//   DISCONNECT → 통화 종료
//
// 스레드 안전: handleDtmf()는 PJSIP 콜백 스레드에서 호출됨 — state_mutex_로 보호
class IvrManager
{
public:
    enum class State
    {
        IDLE = 0,    // 초기 상태
        MENU,        // 메뉴 안내 중 / 입력 대기
        AI_CHAT,     // AI 대화 모드 활성
        TRANSFER,    // 상담원 연결 처리 중
        DISCONNECT,  // 통화 종료 처리 중
    };

    explicit IvrManager(const std::string& session_id);
    ~IvrManager() = default;

    // PJSIP onDtmfDigit() 콜백에서 호출
    // digit: 단일 문자 문자열 ("0"-"9", "*", "#")
    void handleDtmf(const std::string& digit);

    // 현재 상태 조회
    State getCurrentState() const;

    // AI 엔진으로 DTMF 포워딩 콜백 등록
    // 인자: digit 문자열
    void setDtmfForwardCallback(std::function<void(const std::string&)> cb);

    // 상담원 연결 요청 콜백 (BYE 또는 REFER)
    void setTransferCallback(std::function<void()> cb);

    // 통화 종료 요청 콜백
    void setDisconnectCallback(std::function<void()> cb);

    // 메뉴 반복 재생 요청 콜백 (TTS 재생 등)
    void setRepeatMenuCallback(std::function<void()> cb);

    // AI 대화 모드 진입 콜백
    void setEnterAiChatCallback(std::function<void()> cb);

    // 상태를 MENU로 전환 (통화 연결 후 최초 진입)
    void activateMenu();

    // IVR 상태머신 초기화
    void reset();

    static const char* stateName(State s);

private:
    // [P1-Deadlock Fix] 콜백을 직접 호출하지 않고 out 파라미터로 반환
    // caller(handleDtmf)가 뮤텍스 해제 후 콜백을 실행
    void processMenuDigit(const std::string& digit, std::function<void()>& out_cb,
                          std::function<void(const std::string&)>& out_dtmf_cb,
                          std::string& out_digit);
    void processAiChatDigit(const std::string& digit, std::function<void()>& out_cb,
                            std::function<void(const std::string&)>& out_dtmf_cb,
                            std::string& out_digit);

    std::string session_id_;
    State current_state_{State::IDLE};
    mutable std::mutex state_mutex_;

    std::function<void(const std::string&)> on_dtmf_forward_;
    std::function<void()> on_transfer_;
    std::function<void()> on_disconnect_;
    std::function<void()> on_repeat_menu_;
    std::function<void()> on_enter_ai_chat_;
};
