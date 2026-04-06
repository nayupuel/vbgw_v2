#pragma once
#include <pjsua2.hpp>

#include <memory>
#include <string>
#include <vector>

class VoicebotEndpoint
{
public:
    VoicebotEndpoint();
    ~VoicebotEndpoint();

    bool init();
    bool start(int sip_port);
    void shutdown();
    pj::TransportId preferredTransportId() const;

    // [E-4] 코덱 우선순위 변경
    void setCodecPriority(const std::string& codec_id, short priority);

private:
    bool startTransport(pjsip_transport_type_e type, const pj::TransportConfig& cfg,
                        const std::string& label, pj::TransportId* out_id);
    void choosePreferredTransport();

    std::unique_ptr<pj::Endpoint> ep_;
    pj::TransportId udp_transport_id_ = PJSUA_INVALID_ID;
    pj::TransportId tcp_transport_id_ = PJSUA_INVALID_ID;
    pj::TransportId tls_transport_id_ = PJSUA_INVALID_ID;
    pj::TransportId preferred_transport_id_ = PJSUA_INVALID_ID;
    bool destroyed_ = false;  // [A-2 Fix] libDestroy() 이중 호출 방지
};
