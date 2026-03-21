#include "engine/VoicebotEndpoint.h"
#include "engine/VoicebotAccount.h"
#include "engine/SessionManager.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>
#include <spdlog/spdlog.h>
#include <atomic>

std::atomic<bool> keep_running(true);

void signalHandler(int signum) {
    std::cout << "\n[알림] 시스템 종료 시그널(" << signum << ") 감지. Graceful Shutdown 진행 중..." << std::endl;
    keep_running = false;
}

int main() {
    // SIGINT (Ctrl+C), SIGTERM 캡처
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    std::cout << "Starting AI Voicebot Gateway (PJSUA2)..." << std::endl;

    VoicebotEndpoint ep;
    if (!ep.init()) {
        std::cerr << "Initialization failed." << std::endl;
        return 1;
    }

    ep.start(5060); // Listen on SIP port 5060

    pj::AccountConfig acc_cfg;
    
    // 환경변수를 통한 PBX 동적 등록 지원 (Asterisk, FreePBX 연동용)
    const char* pbx_uri = std::getenv("PBX_URI");
    const char* pbx_id_uri = std::getenv("PBX_ID_URI");
    const char* pbx_username = std::getenv("PBX_USERNAME");
    const char* pbx_password = std::getenv("PBX_PASSWORD");

    if (pbx_uri && pbx_id_uri && pbx_username && pbx_password) {
        acc_cfg.idUri = pbx_id_uri;
        acc_cfg.regConfig.registrarUri = pbx_uri;
        acc_cfg.sipConfig.authCreds.push_back(
            pj::AuthCredInfo("digest", "*", pbx_username, 0, pbx_password)
        );
        spdlog::info("[VBGW] PBX Registration Mode Enabled.");
        spdlog::info("       - Registrar: {}", pbx_uri);
        spdlog::info("       - ID URI: {}", pbx_id_uri);
    } else {
        acc_cfg.idUri = "sip:voicebot@127.0.0.1";
        spdlog::info("[VBGW] Local Mode Enabled (No PBX). Direct IP calls: {}", acc_cfg.idUri);
    }
    
    // 계정 생성 및 PBX REG 등록
    VoicebotAccount acc;
    try {
        acc.create(acc_cfg);
        spdlog::info("Voicebot Account created. Listening or Registered to PBX successfully!");
    } catch(pj::Error& err) {
        std::cerr << "Error creating account: " << err.info() << std::endl;
        return 1;
    }

    std::cout << "Press Ctrl+C to stop the gateway gracefully." << std::endl;

    // 데몬 루프
    while (keep_running) {
        // PJSIP의 이벤트 워커 스레드가 비동기로 동작하므로 
        // 메인 스레드는 슬립 상태로 유지하며 시그널을 대기합니다.
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // 1. 진행 중인 모든 통화 강제 종료 (PBX에 BYE 전송)
    std::cout << "[Shutdown 1/3] Hanging up all active calls..." << std::endl;
    SessionManager::getInstance().hangupAllCalls();

    // 네트워크 상으로 BYE 패킷이 안전하게 전송될 시간을 확보
    // PJSIP가 모든 Call 레코드를 정상 삭제할 때까지 대기어 (최대 5초 제한)
    int max_wait_ms = 5000;
    while (SessionManager::getInstance().getActiveCallCount() > 0 && max_wait_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        max_wait_ms -= 100;
    }

    // 2. 로컬 SIP 포트 닫기 및 계정 정리
    std::cout << "[Shutdown 2/3] Destroying account..." << std::endl;
    acc.modify(acc_cfg); // 내부 자원 회수 유도

    // 3. PJSIP 엔진 완벽히 내리기
    std::cout << "[Shutdown 3/3] Shutting down PJLIB..." << std::endl;
    ep.shutdown();

    std::cout << "Gateway shutdown complete. Goodbye!" << std::endl;
    return 0;
}
