#include "VoicebotEndpoint.h"

#include "../utils/AppConfig.h"

#include <spdlog/spdlog.h>

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

        // [H-1 Note] RTP 포트 범위는 PJSUA2 C++ API(MediaConfig)에서 직접 지원하지 않음
        // pjsua_media_config의 snd_port 또는 AccountConfig.mediaConfig.transportConfig에서
        // port/portRange를 통해 개별 설정 가능. 현재는 OS 임의 할당을 사용하며,
        // 방화벽 설정이 필요 시 iptables/nftables에서 PJSIP 기본 범위를 허용해야 함.
        spdlog::info("[Endpoint] RTP port range config: {}-{} (note: requires AccountConfig level "
                     "setup for full control)",
                     cfg.rtp_port_min, cfg.rtp_port_max);

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
        TransportConfig tcfg;
        tcfg.port = sip_port;

        // [CR-3 Fix] SIP TLS 트랜스포트 지원
        // SIP_USE_TLS=1 시 PJSIP_TRANSPORT_TLS 사용
        // 실제 TLS 인증서가 없으면 디폴트 UDP로 유지
        if (cfg.sip_use_tls) {
            if (!cfg.sip_tls_cert_file.empty()) {
                tcfg.tlsConfig.certFile = cfg.sip_tls_cert_file;
            }
            if (!cfg.sip_tls_privkey_file.empty()) {
                tcfg.tlsConfig.privKeyFile = cfg.sip_tls_privkey_file;
            }
            if (!cfg.sip_tls_ca_file.empty()) {
                tcfg.tlsConfig.CaListFile = cfg.sip_tls_ca_file;
            }
            ep_->transportCreate(PJSIP_TRANSPORT_TLS, tcfg);
            spdlog::info("[Endpoint] SIP TLS transport started on port {}", sip_port);
        } else {
            ep_->transportCreate(PJSIP_TRANSPORT_UDP, tcfg);
            spdlog::info("[Endpoint] SIP UDP transport started on port {}", sip_port);
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
