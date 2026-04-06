#include "api/HttpServer.h"
#include "engine/SessionManager.h"
#include "engine/VoicebotAccount.h"
#include "engine/VoicebotEndpoint.h"
#include "utils/AppConfig.h"
#include "utils/RuntimeMetrics.h"

#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <thread>
#include <vector>

// 전역 종료 플래그 (SIGINT 등 인터럽트 기반 안전 종료 대기용)
std::atomic<bool> keep_running(true);

// [CR-2 Fix] 시그널 핸들러: async-signal-safe 함수만 사용
// std::cout/cerr은 async-signal-safe가 아니므로 제거
void signalHandler(int /* signum */)
{
    keep_running.store(false, std::memory_order_release);
}

static pjsua_100rel_use parsePrackMode(const std::string& mode_raw)
{
    std::string mode = mode_raw;
    for (char& c : mode) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (mode == "mandatory") {
        return PJSUA_100REL_MANDATORY;
    }
    if (mode == "optional") {
        return PJSUA_100REL_OPTIONAL;
    }
    return PJSUA_100REL_NOT_USED;
}

static pjsua_sip_timer_use parseSessionTimerMode(const std::string& mode_raw)
{
    std::string mode = mode_raw;
    for (char& c : mode) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (mode == "inactive") {
        return PJSUA_SIP_TIMER_INACTIVE;
    }
    if (mode == "required") {
        return PJSUA_SIP_TIMER_REQUIRED;
    }
    if (mode == "always") {
        return PJSUA_SIP_TIMER_ALWAYS;
    }
    return PJSUA_SIP_TIMER_OPTIONAL;
}

int main()
{
    // [CR-2 Fix] signal() → sigaction() 전환
    // signal()은 POSIX에서 동작이 구현체 의존(UB)이며,
    // 핸들러 실행 중 동일 시그널 재수신 시 경쟁 조건 발생
    struct sigaction sa{};
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // [H-6 Fix] AppConfig 싱글톤 초기화 — 모든 환경변수를 1회 읽어 캐싱
    const auto& cfg = AppConfig::instance();

    // [O4-1] 멀티싱크 로거: 콘솔 + 파일 (LOG_DIR 환경변수 설정 시 활성)
    // 파일 로그: 10MB 롤링, 최대 5개 보관
    auto log_level = spdlog::level::from_str(cfg.log_level);

    std::vector<spdlog::sink_ptr> sinks;
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");
    sinks.push_back(console_sink);

    if (!cfg.log_dir.empty()) {
        std::filesystem::create_directories(cfg.log_dir);
        // daily_file_sink_mt creates files like vbgw_YYYY-MM-DD.log and rotates at 00:00 midnight
        std::string log_path = cfg.log_dir + "/vbgw.log";
        auto file_sink = std::make_shared<spdlog::sinks::daily_file_sink_mt>(log_path, 0, 0);
        file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");
        sinks.push_back(file_sink);
    }

    auto logger = std::make_shared<spdlog::logger>("vbgw", sinks.begin(), sinks.end());
    logger->set_level(log_level);
    spdlog::set_default_logger(logger);

    spdlog::info("Starting AI Voicebot Gateway (PJSUA2)... [profile={}, log_level={}{}]",
                 cfg.runtime_profile, cfg.log_level,
                 cfg.log_dir.empty() ? "" : std::string(", log_dir=") + cfg.log_dir);

    std::vector<std::string> security_errors;
    if (!cfg.validateRuntimeSecurityPolicy(&security_errors)) {
        spdlog::critical("[Security] Runtime security policy validation failed:");
        for (const auto& err : security_errors) {
            spdlog::critical("  - {}", err);
        }
        return 2;
    }

    VoicebotEndpoint ep;
    if (!ep.init()) {
        spdlog::critical("Initialization failed. Aborting.");
        return 1;
    }

    // [E-4 Fix] 광대역 HD Voice 코덱(Opus, G.722) 우선순위 상향
    try {
        ep.setCodecPriority("opus/48000/2", 255);
        ep.setCodecPriority("opus/48000/1", 254);
        ep.setCodecPriority("G722/16000/1", 253);
        spdlog::info("[VBGW] Codec priorities updated (Opus/G722 preferred)");
    } catch (pj::Error& err) {
        spdlog::warn("[VBGW] Some codecs not found, skipping priority update: {}", err.info());
    }

    if (!ep.start(cfg.sip_port)) {
        return 1;
    }

    pj::AccountConfig acc_cfg;

    // 멀티 트랜스포트 중 우선 전송계층 바인딩 (NAT/SIP outbound 안정화)
    const auto preferred_tp = ep.preferredTransportId();
    if (preferred_tp != PJSUA_INVALID_ID) {
        acc_cfg.sipConfig.transportId = preferred_tp;
    }

    // NAT traversal 설정
    acc_cfg.natConfig.contactRewriteUse =
        cfg.sip_nat_contact_rewrite_enable ? cfg.sip_nat_contact_rewrite_mode : 0;
    acc_cfg.natConfig.viaRewriteUse = cfg.sip_nat_via_rewrite_enable ? PJ_TRUE : PJ_FALSE;
    acc_cfg.natConfig.sdpNatRewriteUse = cfg.sip_nat_sdp_rewrite_enable ? PJ_TRUE : PJ_FALSE;
    acc_cfg.natConfig.sipOutboundUse = cfg.sip_nat_sip_outbound_enable ? PJ_TRUE : PJ_FALSE;
    acc_cfg.natConfig.udpKaIntervalSec =
        static_cast<unsigned>(std::max(0, cfg.sip_udp_keepalive_interval_secs));
    acc_cfg.natConfig.sipStunUse =
        cfg.sip_stun_sip_enable ? PJSUA_STUN_USE_DEFAULT : PJSUA_STUN_USE_DISABLED;
    acc_cfg.natConfig.mediaStunUse =
        cfg.sip_stun_media_enable ? PJSUA_STUN_USE_DEFAULT : PJSUA_STUN_USE_DISABLED;
    acc_cfg.natConfig.iceEnabled = cfg.sip_ice_enable;
    acc_cfg.natConfig.turnEnabled = cfg.sip_turn_enable;
    if (cfg.sip_turn_enable) {
        acc_cfg.natConfig.turnServer = cfg.sip_turn_server;
        acc_cfg.natConfig.turnUserName = cfg.sip_turn_username;
        acc_cfg.natConfig.turnPasswordType = 0;
        acc_cfg.natConfig.turnPassword = cfg.sip_turn_password;
        acc_cfg.natConfig.turnConnType = PJ_TURN_TP_UDP;
    }

    // PRACK + Session Timer 설정
    acc_cfg.callConfig.prackUse = parsePrackMode(cfg.sip_prack_mode);
    acc_cfg.callConfig.timerUse = parseSessionTimerMode(cfg.sip_session_timer_mode);
    acc_cfg.callConfig.timerMinSESec = static_cast<unsigned>(cfg.sip_timer_min_se_secs);
    acc_cfg.callConfig.timerSessExpiresSec = static_cast<unsigned>(cfg.sip_timer_sess_expires_secs);

    // RTP/RTCP 운용 설정
    acc_cfg.mediaConfig.streamKaEnabled = cfg.rtp_stream_keepalive_enable;
    acc_cfg.mediaConfig.rtcpMuxEnabled = cfg.rtp_rtcp_mux_enable;
    acc_cfg.mediaConfig.rtcpXrEnabled = cfg.rtp_rtcp_xr_enable;
    if (cfg.rtp_rtcp_fb_nack_enable) {
        pj::RtcpFbCap nack_cap;
        nack_cap.codecId = "*";
        nack_cap.type = PJMEDIA_RTCP_FB_NACK;
        acc_cfg.mediaConfig.rtcpFbConfig.caps.push_back(nack_cap);
    }

    // [CR-3 + Sprint2] SRTP 활성화/강제화
    if (cfg.srtp_enable) {
        const bool use_mandatory = cfg.srtp_mandatory || cfg.isProductionProfile();
        acc_cfg.mediaConfig.srtpUse =
            use_mandatory ? PJMEDIA_SRTP_MANDATORY : PJMEDIA_SRTP_OPTIONAL;
        acc_cfg.mediaConfig.srtpSecureSignaling = cfg.sip_transport_tls_enable ? 1 : 0;

        spdlog::info("[VBGW] SRTP is ENABLED ({})", use_mandatory ? "mandatory" : "optional");
        if (cfg.sip_transport_tls_enable) {
            spdlog::info("       - secure signaling policy: TLS");
        } else {
            spdlog::warn("       - SRTP over UDP (Not recommended for Production)");
        }
    }

    // [H-1 Fix] 고정 RTP 포트 범위 설정
    acc_cfg.mediaConfig.transportConfig.portRange = cfg.rtp_port_max - cfg.rtp_port_min;
    acc_cfg.mediaConfig.transportConfig.port = cfg.rtp_port_min;

    if (cfg.pbx_mode) {
        RuntimeMetrics::instance().setSipMode(true);

        acc_cfg.idUri = cfg.pbx_id_uri;

        if (cfg.sip_register_enable) {
            acc_cfg.regConfig.registrarUri = cfg.pbx_uri;
            // [M-3 Fix] PBX 등록 끊김 시 PJSIP 자동 재등록 시도 간격(초)
            acc_cfg.regConfig.retryIntervalSec = 60;
            spdlog::info("[VBGW] PBX Registration Mode Enabled.");
            spdlog::info("       - Registrar: {}", cfg.pbx_uri);
        } else {
            spdlog::info("[VBGW] SBC Trunk Mode Enabled (No Registration).");
            spdlog::info("       - Trunk IP expected at: {}", cfg.pbx_uri);
        }

        spdlog::info("       - ID URI: {}", cfg.pbx_id_uri);

        acc_cfg.sipConfig.authCreds.push_back(
            pj::AuthCredInfo("digest", "*", cfg.pbx_username, 0, cfg.pbx_password));
    } else {
        RuntimeMetrics::instance().setSipMode(false);
        RuntimeMetrics::instance().setSipRegistration(true, 200);

        acc_cfg.idUri = "sip:voicebot@127.0.0.1";
        spdlog::info("[VBGW] Local Mode Enabled (No PBX). Direct IP calls: {}", acc_cfg.idUri);
    }

    // 계정 생성 및 PBX REG 등록
    VoicebotAccount acc;
    try {
        acc.create(acc_cfg);
        spdlog::info("Voicebot Account created. Listening or Registered to PBX successfully!");
    } catch (pj::Error& err) {
        spdlog::error("Error creating account: {}", err.info());
        return 1;
    }
    HttpServer::getInstance().setAccount(&acc);

    // [H-4, H-5, E-2 Fix] 내장 관리 서버 시작
    if (!HttpServer::getInstance().start(cfg.http_port)) {
        spdlog::error("[VBGW] Failed to start HTTP Admin Server on port {}", cfg.http_port);
    }

    spdlog::info("Press Ctrl+C to stop the gateway gracefully.");

    // [P2-1 Fix] 녹음 파일 보관 주기 / 용량 관리 함수
    auto cleanUpRecordings = [&cfg]() {
        if (!cfg.call_recording_enable || cfg.call_recording_dir.empty())
            return;

        std::error_code ec;
        if (!std::filesystem::exists(cfg.call_recording_dir, ec))
            return;

        try {
            auto now = std::filesystem::file_time_type::clock::now();
            auto max_age = std::chrono::hours(24 * cfg.call_recording_max_days);

            // 파일 리스트 및 크기 수집
            struct FileInfo
            {
                std::filesystem::path path;
                std::filesystem::file_time_type last_write_time;
                uintmax_t size;
            };
            std::vector<FileInfo> files;
            uintmax_t total_size = 0;

            for (const auto& entry : std::filesystem::directory_iterator(cfg.call_recording_dir)) {
                if (entry.is_regular_file()) {
                    auto lwt = entry.last_write_time();
                    // 1. 기간(Days) 지난 파일 즉시 삭제
                    if (now - lwt > max_age) {
                        std::filesystem::remove(entry.path(), ec);
                        spdlog::info("[Cleanup] Removed old recording: {}", entry.path().string());
                    } else {
                        uintmax_t size = entry.file_size(ec);
                        files.push_back({entry.path(), lwt, size});
                        total_size += size;
                    }
                }
            }

            // 2. 용량(MB) 초과 시 가장 오래된 파일부터 삭제
            uintmax_t max_size_bytes =
                static_cast<uintmax_t>(cfg.call_recording_max_mb) * 1024 * 1024;
            if (total_size > max_size_bytes) {
                // 오름차순 정렬 (오래된 것부터)
                std::sort(files.begin(), files.end(), [](const FileInfo& a, const FileInfo& b) {
                    return a.last_write_time < b.last_write_time;
                });

                for (const auto& f : files) {
                    if (total_size <= max_size_bytes)
                        break;
                    std::filesystem::remove(f.path, ec);
                    total_size -= f.size;
                    spdlog::info("[Cleanup] Removed recording due to quota ({}MB): {}",
                                 cfg.call_recording_max_mb, f.path.string());
                }
            }
        } catch (const std::exception& e) {
            spdlog::warn("[Cleanup] Recording cleanup error: {}", e.what());
        }
    };

    // 데몬 루프
    auto last_clean_time = std::chrono::steady_clock::now();
    // 시작 시 1회 실행
    cleanUpRecordings();

    while (keep_running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::hours>(now - last_clean_time).count() >= 1) {
            cleanUpRecordings();
            last_clean_time = now;
        }
    }

    // [R-3 Fix] 5단계 Graceful Shutdown 시퀀스 (VAD-v5/Shutdown Fix)
    // 객체 파괴 순서: HTTP → 콜 → AI세션 → SessionManager → Account → Endpoint
    // 핵심 원칙: PJSIP ep.shutdown() 호출 전에 모든 PJSIP 종속 객체가 먼저 정리되어야 함

    // 1단계: HTTP Admin 즉시 중단 — 신규 Outbound Call 수신 차단
    spdlog::info("[Shutdown 1/5] Stopping HTTP Admin Server...");
    HttpServer::getInstance().stop();
    HttpServer::getInstance().setAccount(nullptr);

    // 2단계: 진행 중인 모든 통화 강제 종료 (PBX에 BYE 전송)
    spdlog::info("[Shutdown 2/5] Hanging up all active calls...");
    SessionManager::getInstance().hangupAllCalls();

    // 네트워크 상으로 BYE 패킷이 안전하게 전송될 시간을 확보
    int max_wait_ms = 3000;
    while (SessionManager::getInstance().getActiveCallCount() > 0 && max_wait_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        max_wait_ms -= 100;
    }
    if (SessionManager::getInstance().getActiveCallCount() > 0) {
        spdlog::warn("[Shutdown] {} calls still active after BYE timeout",
                     SessionManager::getInstance().getActiveCallCount());
    }

    // 3단계: 모든 콜의 AI gRPC 세션 명시적 종료
    spdlog::info("[Shutdown 3/5] Ending all AI sessions...");
    SessionManager::getInstance().endAllAiSessions();

    // 4단계: SessionManager의 모든 콜 shared_ptr 해제
    // — VoicebotCall/VoicebotAiClient 소멸자가 여기서 실행됨
    // — PJSIP ep가 아직 살아있으므로 소멸자 내 PJSIP API 호출이 안전
    spdlog::info("[Shutdown 4/5] Clearing all call references...");
    SessionManager::getInstance().clearAllCalls();

    // 5단계: PJSIP Account/Endpoint 종료
    // acc의 소멸자가 main() 종료 시 호출되므로, ep.shutdown() 전에
    // Account를 명시적으로 정리하여 소멸자-PJSIP 경합 방지
    spdlog::info("[Shutdown 5/5] Shutting down SIP/PJLIB...");
    try {
        acc.shutdown();
    } catch (...) {
        // acc.shutdown()은 PJSIP 내부 에러를 발생시킬 수 있음 — 무시
    }

    ep.shutdown();

    spdlog::info("Gateway shutdown complete. Goodbye!");
    return 0;
}
