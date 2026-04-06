#include "VoicebotEndpoint.h"

#include "../utils/AppConfig.h"

#include <spdlog/spdlog.h>

#include <cctype>
#include <cstdlib>

using namespace pj;

// [M-6 Fix] ep.reset(new Endpoint) → make_unique
VoicebotEndpoint::VoicebotEndpoint() : ep_(std::make_unique<Endpoint>()) {}

VoicebotEndpoint::~VoicebotEndpoint()
{
    shutdown();
}

bool VoicebotEndpoint::init()
{
    const auto& cfg = AppConfig::instance();

    try {
        ep_->libCreate();
        EpConfig ep_cfg;

        // [O4-2] PJSIP 내부 로그 레벨 — AppConfig에서 캐싱된 값 사용
        ep_cfg.logConfig.level = cfg.pjsip_log_level;
        ep_cfg.logConfig.consoleLevel = cfg.pjsip_log_level;
        ep_cfg.uaConfig.maxCalls = static_cast<unsigned>(cfg.max_concurrent_calls);

        // STUN 서버 설정 (NAT traversal)
        if (!cfg.sip_stun_server.empty()) {
            ep_cfg.uaConfig.stunServer.push_back(cfg.sip_stun_server);
            spdlog::info("[Endpoint] STUN server configured: {}", cfg.sip_stun_server);
        }

        // [H-1 Note] RTP 포트 범위는 PJSUA2 C++ API(MediaConfig)에서 직접 지원하지 않음
        // pjsua_media_config의 snd_port 또는 AccountConfig.mediaConfig.transportConfig에서
        // port/portRange를 통해 개별 설정 가능. 현재는 OS 임의 할당을 사용하며,
        // 방화벽 설정이 필요 시 iptables/nftables에서 PJSIP 기본 범위를 허용해야 함.
        spdlog::info("[Endpoint] RTP port range config: {}-{} (note: requires AccountConfig level "
                     "setup for full control)",
                     cfg.rtp_port_min, cfg.rtp_port_max);

        // [JB] Jitter Buffer 명시적 설정 — libInit() 전에 반드시 적용
        // 단위: ms. -1 이면 PJSIP 자동 선택.
        // jbMinPre ≤ jbInit ≤ jbMaxPre ≤ jbMax 순서 보장 (AppConfig에서 검증 완료)
        ep_cfg.medConfig.jbInit = cfg.jb_init_ms;
        ep_cfg.medConfig.jbMinPre = cfg.jb_min_pre_ms;
        ep_cfg.medConfig.jbMaxPre = cfg.jb_max_pre_ms;
        ep_cfg.medConfig.jbMax = cfg.jb_max_ms;
        spdlog::info(
            "[Endpoint] Jitter Buffer config: init={}ms, minPre={}ms, maxPre={}ms, max={}ms",
            cfg.jb_init_ms, cfg.jb_min_pre_ms, cfg.jb_max_pre_ms, cfg.jb_max_ms);

        ep_->libInit(ep_cfg);

        if (cfg.pjsip_null_audio) {
            ep_->audDevManager().setNullDev();
            spdlog::info("[Endpoint] PJSIP null-audio device enabled (PJSIP_NULL_AUDIO=1)");
        }

        spdlog::info("[Endpoint] PJSIP initialized [pjsip_log_level={}]", cfg.pjsip_log_level);
        return true;
    } catch (Error& err) {
        spdlog::error("[Endpoint] Initialization error: {}", err.info());
        return false;
    }
}

bool VoicebotEndpoint::start(int sip_port)
{
    const auto& cfg = AppConfig::instance();

    try {
        TransportConfig base_tcfg;
        base_tcfg.port = sip_port;

        TransportConfig tls_tcfg = base_tcfg;
        if (!cfg.sip_tls_cert_file.empty()) {
            tls_tcfg.tlsConfig.certFile = cfg.sip_tls_cert_file;
        }
        if (!cfg.sip_tls_privkey_file.empty()) {
            tls_tcfg.tlsConfig.privKeyFile = cfg.sip_tls_privkey_file;
        }
        if (!cfg.sip_tls_ca_file.empty()) {
            tls_tcfg.tlsConfig.CaListFile = cfg.sip_tls_ca_file;
        }

        bool started_any = false;
        if (cfg.sip_transport_udp_enable) {
            started_any |=
                startTransport(PJSIP_TRANSPORT_UDP, base_tcfg, "UDP", &udp_transport_id_);
        }
        if (cfg.sip_transport_tcp_enable) {
            started_any |=
                startTransport(PJSIP_TRANSPORT_TCP, base_tcfg, "TCP", &tcp_transport_id_);
        }
        if (cfg.sip_transport_tls_enable) {
            started_any |= startTransport(PJSIP_TRANSPORT_TLS, tls_tcfg, "TLS", &tls_transport_id_);
        }
        if (!started_any) {
            spdlog::critical("[Endpoint] No SIP transport could be started");
            return false;
        }

        choosePreferredTransport();
        if (preferred_transport_id_ != PJSUA_INVALID_ID) {
            try {
                const auto ti = ep_->transportGetInfo(preferred_transport_id_);
                spdlog::info("[Endpoint] Preferred SIP transport: id={} type={} addr={}",
                             preferred_transport_id_, static_cast<int>(ti.type), ti.localName);
            } catch (...) {
                spdlog::info("[Endpoint] Preferred SIP transport id={}", preferred_transport_id_);
            }
        }

        ep_->libStart();
        return true;
    } catch (Error& err) {
        spdlog::critical("[Endpoint] Failed to start SIP transport on port {}: {}", sip_port,
                         err.info());
        return false;
    }
}

void VoicebotEndpoint::shutdown()
{
    // [A-2 Fix] libDestroy() 이중 호출 방지
    // 소멸자와 main()에서 각각 호출될 수 있으므로 플래그로 보호
    if (destroyed_) {
        return;
    }
    destroyed_ = true;

    try {
        ep_->libDestroy();
    } catch (Error& err) {
        // [M-5 Fix] std::cerr → spdlog (로그 파일에도 기록되도록)
        spdlog::error("[Endpoint] Shutdown error: {}", err.info());
    }
}

void VoicebotEndpoint::setCodecPriority(const std::string& codec_id, short priority)
{
    if (ep_) {
        // PJSIP의 pj_str_t 사용으로 캐스팅
        ep_->codecSetPriority(codec_id, priority);
    }
}

bool VoicebotEndpoint::startTransport(pjsip_transport_type_e type, const pj::TransportConfig& cfg,
                                      const std::string& label, pj::TransportId* out_id)
{
    try {
        const auto id = ep_->transportCreate(type, cfg);
        if (out_id) {
            *out_id = id;
        }
        spdlog::info("[Endpoint] SIP {} transport started [id={}, port={}]", label, id, cfg.port);
        return true;
    } catch (const Error& err) {
        spdlog::error("[Endpoint] Failed to start SIP {} transport on port {}: {}", label, cfg.port,
                      err.info());
        if (out_id) {
            *out_id = PJSUA_INVALID_ID;
        }
        return false;
    }
}

void VoicebotEndpoint::choosePreferredTransport()
{
    const auto& cfg = AppConfig::instance();
    std::string pref = cfg.sip_transport_preferred;
    for (char& ch : pref) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }

    if (pref == "tls" && tls_transport_id_ != PJSUA_INVALID_ID) {
        preferred_transport_id_ = tls_transport_id_;
        return;
    }
    if (pref == "tcp" && tcp_transport_id_ != PJSUA_INVALID_ID) {
        preferred_transport_id_ = tcp_transport_id_;
        return;
    }
    if (pref == "udp" && udp_transport_id_ != PJSUA_INVALID_ID) {
        preferred_transport_id_ = udp_transport_id_;
        return;
    }

    if (cfg.sip_transport_tls_enable && tls_transport_id_ != PJSUA_INVALID_ID) {
        preferred_transport_id_ = tls_transport_id_;
    } else if (cfg.sip_transport_tcp_enable && tcp_transport_id_ != PJSUA_INVALID_ID) {
        preferred_transport_id_ = tcp_transport_id_;
    } else {
        preferred_transport_id_ = udp_transport_id_;
    }
}

pj::TransportId VoicebotEndpoint::preferredTransportId() const
{
    return preferred_transport_id_;
}
