#include "VoicebotCall.h"

#include "../ai/VoicebotAiClient.h"
#include "../ivr/IvrManager.h"
#include "../utils/AppConfig.h"
#include "../utils/RuntimeMetrics.h"
#include "SessionManager.h"
#include "VoicebotAccount.h"
#include "VoicebotMediaPort.h"

#include <pjlib.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <random>
#include <sstream>

// [T-3 Fix] UUID v4 생성기 — std::random_device로 예측 불가능한 시드 사용
// 분산 환경에서 두 Gateway 인스턴스가 동시 시작해도 세션 ID 충돌 방지
static std::string generateSessionId()
{
    static thread_local std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);

    // 간이 UUID v4: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
    std::ostringstream oss;
    oss << std::hex;
    oss << dist(gen) << "-";
    oss << (dist(gen) & 0xFFFF) << "-";
    // UUID v4: version 4 표시
    oss << ((dist(gen) & 0x0FFF) | 0x4000) << "-";
    // variant bits
    oss << ((dist(gen) & 0x3FFF) | 0x8000) << "-";
    oss << dist(gen) << (dist(gen) & 0xFFFF);
    return oss.str();
}

void ensurePjThreadRegistered(const char* thread_name)
{
    thread_local pj_thread_desc thread_desc;
    thread_local pj_thread_t* pj_thread = nullptr;
    if (!pj_thread_is_registered()) {
        pj_thread_register(thread_name, thread_desc, &pj_thread);
    }
}

VoicebotCall::VoicebotCall(pj::Account& acc, int call_id)
    : pj::Call(acc, call_id),
      media_port_(nullptr),
      ai_client_(nullptr),
      ivr_manager_(nullptr),
      recorder_(nullptr)
{
    start_time_ = std::chrono::system_clock::now();
    session_id_ = generateSessionId();

    // [P1-Safety Fix] IVR 상태머신 생성만 수행
    // 콜백은 onCallMediaState()에서 shared_ptr 확보 후 바인딩
    // 생성자 시점에서는 shared_from_this() 불가능하므로 [this] 캡처 위험
    ivr_manager_ = std::make_unique<IvrManager>(session_id_);
}

VoicebotCall::~VoicebotCall()
{
    std::string ignored;
    stopRecording(&ignored);

    // ai_client_ endSession은 소멸자 호출 전 onCallState(DISCONNECTED)에서
    // SessionManager::removeCall()을 통해 shared_ptr refcount가 0이 되면 자동 수행됨
}

void VoicebotCall::onCallState(pj::OnCallStateParam& prm)
{
    pj::CallInfo ci = getInfo();
    spdlog::info("[Call] ID={} Session={} State={} Reason={}", ci.id, session_id_, ci.stateText,
                 ci.lastReason);

    if (ci.state == PJSIP_INV_STATE_DISCONNECTED) {
        std::string ignored;
        stopRecording(&ignored);
        endAiSession();
        dumpCdr(ci.lastReason);
        SessionManager::getInstance().removeCall(ci.id);
        spdlog::info("[Call] ID={} Session={} Removed from SessionManager.", ci.id, session_id_);
    }
}

void VoicebotCall::onCallTsxState(pj::OnCallTsxStateParam& prm)
{
    PJ_UNUSED_ARG(prm);
    try {
        const auto ci = getInfo();
        spdlog::debug("[Call] Session={} onCallTsxState (state={})", session_id_, ci.stateText);
    } catch (...) {}
}

void VoicebotCall::onCallMediaState(pj::OnCallMediaStateParam& prm)
{
    pj::CallInfo ci = getInfo();
    const auto& cfg = AppConfig::instance();

    for (unsigned i = 0; i < ci.media.size(); ++i) {
        if (ci.media[i].type != PJMEDIA_TYPE_AUDIO)
            continue;

        pj::AudioMedia* aud_med = dynamic_cast<pj::AudioMedia*>(getMedia(i));
        if (!aud_med)
            continue;

        bool needs_hangup = false;
        {
            std::lock_guard<std::mutex> lock(ai_init_mutex_);

            if (!ai_client_) {
                // [CR-1 Fix] gRPC 채널을 AppConfig 싱글톤에서 공유
                // 매 콜마다 TCP + HTTP/2 + TLS 핸드셰이크 비용 제거
                // AI_ENGINE_ADDR 검증도 AppConfig 생성 시 1회만 수행
                spdlog::info("[Call] Connecting to AI Engine at: {}", cfg.ai_engine_addr);

                try {
                    auto channel = cfg.getGrpcChannel();
                    ai_client_ = std::make_shared<VoicebotAiClient>(channel);
                } catch (const std::runtime_error& e) {
                    spdlog::critical("[Call] gRPC channel creation failed: {} — hanging up",
                                     e.what());
                    needs_hangup = true;
                }

                if (!needs_hangup) {
                    if (!cfg.grpc_use_tls) {
                        spdlog::warn(
                            "[Call] gRPC using INSECURE channel — GRPC_USE_TLS=1 is REQUIRED "
                            "for production");
                    }

                    // [M-1 Fix] this 캡처 → weak_from_this() 캡처
                    // AI 클라이언트의 read 스레드가 아직 실행 중일 때
                    // VoicebotCall이 먼저 소멸하면 dangling 접근 발생 방지
                    auto weak_self = weak_from_this();

                    ai_client_->setTtsCallback([weak_self](const uint8_t* data, size_t len) {
                        if (auto self = weak_self.lock()) {
                            if (self->media_port_) {
                                self->media_port_->writeTtsAudio(data, len);
                            }
                        }
                    });

                    ai_client_->setTtsClearCallback([weak_self]() {
                        if (auto self = weak_self.lock()) {
                            self->bargein_count_.fetch_add(1, std::memory_order_relaxed);
                            if (self->media_port_) {
                                self->media_port_->clearTtsAudio();
                                self->media_port_->resetVad();
                            }
                        }
                    });

                    ai_client_->setErrorCallback([weak_self](const std::string& err) {
                        spdlog::error("🚨 [Call] Hanging up due to permanent AI Error: {}", err);
                        if (auto self = weak_self.lock()) {
                            try {
                                pj::CallOpParam prm;
                                prm.statusCode = PJSIP_SC_SERVICE_UNAVAILABLE;
                                self->hangup(prm);
                            } catch (const pj::Error& e) {
                                spdlog::warn("[Call] Error during hangup: {}", e.info());
                            }
                        }
                    });

                    ai_client_->startSession(session_id_);
                }
            }

            if (!needs_hangup && !media_port_) {
                media_port_ = std::make_unique<VoicebotMediaPort>();
                media_port_->setAiClient(ai_client_);
                media_port_->setVadSpeechStartCallback([weak_self = weak_from_this()]() {
                    if (auto self = weak_self.lock()) {
                        self->vad_trigger_count_.fetch_add(1, std::memory_order_relaxed);
                    }
                });
            }
        }  // ai_init_mutex_ released

        if (needs_hangup) {
            try {
                pj::CallOpParam hangup_prm;
                hangup_prm.statusCode = PJSIP_SC_SERVICE_UNAVAILABLE;
                hangup(hangup_prm);
            } catch (...) {}
            return;
        }

        aud_med->startTransmit(*media_port_);
        media_port_->startTransmit(*aud_med);
        {
            std::lock_guard<std::mutex> lock(media_mutex_);
            primary_audio_media_index_ = static_cast<int>(i);
            if (cfg.call_recording_enable && !recording_active_) {
                std::string rec_err;
                if (!startRecordingLocked("", &rec_err)) {
                    spdlog::warn("[Call] Session={} recording auto-start failed: {}", session_id_,
                                 rec_err.empty() ? "unknown error" : rec_err);
                }
            }
        }

        if (ivr_manager_) {
            // [P1-Safety Fix] 콜백은 shared_ptr(this)가 정상인 onCallMediaState()에서
            // weak_from_this()를 사용하여 안전하게 바인딩
            auto weak_self = weak_from_this();

            ivr_manager_->setDtmfForwardCallback([weak_self](const std::string& digit) {
                if (auto self = weak_self.lock()) {
                    std::string ignored;
                    self->sendDtmfToAi(digit, &ignored);
                }
            });

            ivr_manager_->setTransferCallback([weak_self]() {
                if (auto self = weak_self.lock()) {
                    spdlog::info("[IVR] Session={} transfer requested by DTMF", self->session_id_);
                    // TODO: 실제 transferTo 호출 등 필요한 추가 로직 위치
                }
            });

            ivr_manager_->setDisconnectCallback([weak_self]() {
                if (auto self = weak_self.lock()) {
                    try {
                        pj::CallOpParam prm;
                        prm.statusCode = PJSIP_SC_DECLINE;
                        self->hangup(prm);
                    } catch (const pj::Error& e) {
                        spdlog::warn("[IVR] Session={} disconnect request hangup failed: {}",
                                     self->session_id_, e.info());
                    }
                }
            });

            ivr_manager_->activateMenu();
        }

        spdlog::info("[Call] AI Media Port connected. Session={} RTP Stream converting to PCM.",
                     session_id_);
    }
}

void VoicebotCall::onDtmfDigit(pj::OnDtmfDigitParam& prm)
{
    spdlog::info("[IVR] Session={} Received DTMF digit: {}", session_id_, prm.digit);
    if (ivr_manager_) {
        ivr_manager_->handleDtmf(prm.digit);
    } else {
        std::string ignored;
        sendDtmfToAi(prm.digit, &ignored);
    }
}

void VoicebotCall::onCallTransferRequest(pj::OnCallTransferRequestParam& prm)
{
    const auto& cfg = AppConfig::instance();
    if (!cfg.sip_accept_refer) {
        prm.statusCode = PJSIP_SC_NOT_ACCEPTABLE_HERE;
        spdlog::warn("[Call] Session={} REFER rejected (policy disabled), dst={}", session_id_,
                     prm.dstUri);
        return;
    }

    prm.statusCode = PJSIP_SC_ACCEPTED;
    spdlog::info("[Call] Session={} REFER accepted, dst={}", session_id_, prm.dstUri);
}

void VoicebotCall::onCallTransferStatus(pj::OnCallTransferStatusParam& prm)
{
    spdlog::info("[Call] Session={} transfer status={} reason='{}' final={}", session_id_,
                 static_cast<int>(prm.statusCode), prm.reason, prm.finalNotify ? "true" : "false");
}

void VoicebotCall::onCallReplaceRequest(pj::OnCallReplaceRequestParam& prm)
{
    const auto& cfg = AppConfig::instance();
    if (!cfg.sip_accept_replaces) {
        prm.statusCode = PJSIP_SC_NOT_ACCEPTABLE_HERE;
        prm.reason = "Replaces disabled by policy";
        spdlog::warn("[Call] Session={} Replaces rejected by policy", session_id_);
        return;
    }

    prm.statusCode = PJSIP_SC_OK;
    spdlog::info("[Call] Session={} Replaces accepted", session_id_);
}

void VoicebotCall::onCallReplaced(pj::OnCallReplacedParam& prm)
{
    spdlog::info("[Call] Session={} replaced by new_call_id={}", session_id_, prm.newCallId);
}

pjsip_redirect_op VoicebotCall::onCallRedirected(pj::OnCallRedirectedParam& prm)
{
    const auto& cfg = AppConfig::instance();
    if (!cfg.sip_follow_redirect) {
        spdlog::warn("[Call] Session={} redirect target={} rejected (policy)", session_id_,
                     prm.targetUri);
        return PJSIP_REDIRECT_STOP;
    }

    const auto op =
        cfg.sip_redirect_replace_to ? PJSIP_REDIRECT_ACCEPT_REPLACE : PJSIP_REDIRECT_ACCEPT;
    spdlog::info("[Call] Session={} redirect target={} accepted (op={})", session_id_,
                 prm.targetUri, static_cast<int>(op));
    return op;
}

bool VoicebotCall::isValidDtmfDigits(const std::string& digits)
{
    if (digits.empty()) {
        return false;
    }
    for (char c : digits) {
        if ((c >= '0' && c <= '9') || c == '*' || c == '#' || c == 'A' || c == 'B' || c == 'C' ||
            c == 'D' || c == 'a' || c == 'b' || c == 'c' || c == 'd') {
            continue;
        }
        return false;
    }
    return true;
}

bool VoicebotCall::isValidTransferTarget(const std::string& target_uri)
{
    if (target_uri.size() < 8 || target_uri.size() > 256) {
        return false;
    }
    const bool scheme_ok = target_uri.rfind("sip:", 0) == 0 || target_uri.rfind("sips:", 0) == 0;
    if (!scheme_ok || target_uri.find('@') == std::string::npos) {
        return false;
    }
    if (target_uri.find_first_of("\r\n\t <>;\"\'`|&") != std::string::npos) {
        return false;
    }
    return true;
}

bool VoicebotCall::sendDtmfToPeer(const std::string& digits, std::string* error_message)
{
    if (!isValidDtmfDigits(digits)) {
        if (error_message) {
            *error_message = "invalid_dtmf_digits";
        }
        return false;
    }

    ensurePjThreadRegistered("vbgw_call_api");
    try {
        dialDtmf(digits);
        spdlog::info("[Call] Session={} Sent DTMF to peer: {}", session_id_, digits);
        return true;
    } catch (const pj::Error& e) {
        if (error_message) {
            *error_message = e.info();
        }
        spdlog::warn("[Call] Session={} Failed to send DTMF to peer '{}': {}", session_id_, digits,
                     e.info());
        return false;
    }
}

bool VoicebotCall::sendDtmfToAi(const std::string& digits, std::string* error_message)
{
    if (!isValidDtmfDigits(digits)) {
        if (error_message) {
            *error_message = "invalid_dtmf_digits";
        }
        return false;
    }

    std::shared_ptr<VoicebotAiClient> client;
    {
        std::lock_guard<std::mutex> lock(ai_init_mutex_);
        client = ai_client_;
    }
    if (!client) {
        if (error_message) {
            *error_message = "ai_session_not_ready";
        }
        return false;
    }

    for (char c : digits) {
        client->sendDtmf(std::string(1, c));
    }
    spdlog::info("[Call] Session={} Forwarded DTMF to AI: {}", session_id_, digits);
    return true;
}

bool VoicebotCall::transferTo(const std::string& target_uri, std::string* error_message)
{
    if (!isValidTransferTarget(target_uri)) {
        if (error_message) {
            *error_message = "invalid_transfer_target";
        }
        return false;
    }

    ensurePjThreadRegistered("vbgw_call_api");
    try {
        pj::CallOpParam prm;
        xfer(target_uri, prm);
        spdlog::info("[Call] Session={} Blind transfer initiated to {}", session_id_, target_uri);
        return true;
    } catch (const pj::Error& e) {
        if (error_message) {
            *error_message = e.info();
        }
        spdlog::warn("[Call] Session={} Blind transfer failed [{}]: {}", session_id_, target_uri,
                     e.info());
        return false;
    }
}

bool VoicebotCall::startRecordingLocked(const std::string& file_path, std::string* error_message)
{
    if (recording_active_) {
        return true;
    }
    if (primary_audio_media_index_ < 0) {
        if (error_message) {
            *error_message = "audio_media_not_ready";
        }
        return false;
    }

    std::string output_path = file_path;
    if (output_path.empty()) {
        const auto& cfg = AppConfig::instance();
        std::filesystem::path dir(cfg.call_recording_dir.empty() ? "recordings"
                                                                 : cfg.call_recording_dir);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            if (error_message) {
                *error_message = std::string("failed_to_create_recording_dir: ") + ec.message();
            }
            return false;
        }

        auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::ostringstream ss;
        ss << "call_" << session_id_ << "_" << static_cast<long long>(now) << ".wav";
        output_path = (dir / ss.str()).string();
    } else {
        std::filesystem::path out(output_path);
        std::error_code ec;
        if (out.has_parent_path()) {
            std::filesystem::create_directories(out.parent_path(), ec);
        }
        if (ec) {
            if (error_message) {
                *error_message = std::string("failed_to_create_recording_parent: ") + ec.message();
            }
            return false;
        }
    }

    try {
        auto recorder = std::make_unique<pj::AudioMediaRecorder>();
        recorder->createRecorder(output_path, 0, 0, 0);
        auto audio = getAudioMedia(primary_audio_media_index_);
        audio.startTransmit(*recorder);

        recorder_ = std::move(recorder);
        recording_file_path_ = output_path;
        recording_active_ = true;
        spdlog::info("[Call] Session={} recording started: {}", session_id_, recording_file_path_);
        return true;
    } catch (const pj::Error& e) {
        if (error_message) {
            *error_message = e.info();
        }
        return false;
    }
}

bool VoicebotCall::startRecording(const std::string& file_path, std::string* error_message)
{
    ensurePjThreadRegistered("vbgw_call_api");
    std::lock_guard<std::mutex> lock(media_mutex_);
    return startRecordingLocked(file_path, error_message);
}

bool VoicebotCall::stopRecording(std::string* error_message)
{
    ensurePjThreadRegistered("vbgw_call_api");
    std::lock_guard<std::mutex> lock(media_mutex_);
    if (!recording_active_) {
        return true;
    }

    try {
        if (recorder_ && primary_audio_media_index_ >= 0) {
            auto audio = getAudioMedia(primary_audio_media_index_);
            audio.stopTransmit(*recorder_);
        }
    } catch (const pj::Error& e) {
        if (error_message) {
            *error_message = e.info();
        }
        spdlog::warn("[Call] Session={} stop recording stopTransmit failed: {}", session_id_,
                     e.info());
    }

    recorder_.reset();
    recording_active_ = false;
    spdlog::info("[Call] Session={} recording stopped: {}", session_id_, recording_file_path_);
    return true;
}

bool VoicebotCall::isRecording() const
{
    std::lock_guard<std::mutex> lock(media_mutex_);
    return recording_active_;
}

std::string VoicebotCall::recordingFilePath() const
{
    std::lock_guard<std::mutex> lock(media_mutex_);
    return recording_file_path_;
}

bool VoicebotCall::getRtpStatsSnapshot(RtpStatsSnapshot* out, std::string* error_message) const
{
    if (!out) {
        if (error_message) {
            *error_message = "output_null";
        }
        return false;
    }

    ensurePjThreadRegistered("vbgw_call_api");

    try {
        const auto info = getInfo();
        int med_idx = -1;
        {
            std::lock_guard<std::mutex> lock(media_mutex_);
            med_idx = primary_audio_media_index_;
        }
        if (med_idx < 0 || med_idx >= static_cast<int>(info.media.size()) ||
            info.media[med_idx].type != PJMEDIA_TYPE_AUDIO) {
            for (unsigned i = 0; i < info.media.size(); ++i) {
                if (info.media[i].type == PJMEDIA_TYPE_AUDIO) {
                    med_idx = static_cast<int>(i);
                    break;
                }
            }
        }

        if (med_idx < 0) {
            if (error_message) {
                *error_message = "audio_media_not_found";
            }
            return false;
        }

        const auto stat = getStreamStat(static_cast<unsigned>(med_idx));
        const auto tp = getMedTransportInfo(static_cast<unsigned>(med_idx));

        RtpStatsSnapshot snap;
        snap.valid = true;
        snap.media_index = med_idx;
        snap.rx_packets = static_cast<std::uint64_t>(stat.rtcp.rxStat.pkt);
        snap.tx_packets = static_cast<std::uint64_t>(stat.rtcp.txStat.pkt);
        snap.rx_lost = static_cast<std::uint64_t>(stat.rtcp.rxStat.loss);
        snap.rx_discard = static_cast<std::uint64_t>(stat.rtcp.rxStat.discard);
        snap.rx_reorder = static_cast<std::uint64_t>(stat.rtcp.rxStat.reorder);
        snap.rx_dup = static_cast<std::uint64_t>(stat.rtcp.rxStat.dup);
        snap.rx_jitter_mean_usec =
            static_cast<std::uint64_t>(std::max(0, stat.rtcp.rxStat.jitterUsec.mean));
        snap.rtt_mean_usec = static_cast<std::uint64_t>(std::max(0, stat.rtcp.rttUsec.mean));
        snap.jbuf_avg_delay_ms = static_cast<std::uint64_t>(stat.jbuf.avgDelayMsec);
        snap.jbuf_lost = static_cast<std::uint64_t>(stat.jbuf.lost);
        snap.jbuf_discard = static_cast<std::uint64_t>(stat.jbuf.discard);
        snap.src_rtp = tp.srcRtpName;
        snap.src_rtcp = tp.srcRtcpName;

        *out = std::move(snap);
        return true;
    } catch (const pj::Error& e) {
        if (error_message) {
            *error_message = e.info();
        }
        return false;
    }
}

bool VoicebotCall::bridgeWith(const std::shared_ptr<VoicebotCall>& other,
                              std::string* error_message)
{
    if (!other) {
        if (error_message) {
            *error_message = "peer_call_not_found";
        }
        return false;
    }
    if (other.get() == this) {
        if (error_message) {
            *error_message = "cannot_bridge_same_call";
        }
        return false;
    }

    ensurePjThreadRegistered("vbgw_call_api");

    int this_idx = -1;
    int other_idx = -1;
    {
        std::lock_guard<std::mutex> lock(media_mutex_);
        this_idx = primary_audio_media_index_;
    }
    {
        std::lock_guard<std::mutex> lock(other->media_mutex_);
        other_idx = other->primary_audio_media_index_;
    }
    if (this_idx < 0 || other_idx < 0) {
        if (error_message) {
            *error_message = "audio_media_not_ready";
        }
        return false;
    }

    try {
        auto this_audio = getAudioMedia(this_idx);
        auto other_audio = other->getAudioMedia(other_idx);
        this_audio.startTransmit(other_audio);
        other_audio.startTransmit(this_audio);

        // [P2-2 Fix] 양방향 브릿지 시 AI 엔진으로의 음성 입력 차단
        if (media_port_)
            media_port_->setAiPaused(true);
        if (other->media_port_)
            other->media_port_->setAiPaused(true);

        spdlog::info("[Call] Session={} bridged with Session={}", session_id_, other->session_id_);
        return true;
    } catch (const pj::Error& e) {
        if (error_message) {
            *error_message = e.info();
        }
        return false;
    }
}

bool VoicebotCall::unbridgeWith(const std::shared_ptr<VoicebotCall>& other,
                                std::string* error_message)
{
    if (!other) {
        if (error_message) {
            *error_message = "peer_call_not_found";
        }
        return false;
    }
    if (other.get() == this) {
        if (error_message) {
            *error_message = "cannot_unbridge_same_call";
        }
        return false;
    }

    ensurePjThreadRegistered("vbgw_call_api");

    int this_idx = -1;
    int other_idx = -1;
    {
        std::lock_guard<std::mutex> lock(media_mutex_);
        this_idx = primary_audio_media_index_;
    }
    {
        std::lock_guard<std::mutex> lock(other->media_mutex_);
        other_idx = other->primary_audio_media_index_;
    }
    if (this_idx < 0 || other_idx < 0) {
        if (error_message) {
            *error_message = "audio_media_not_ready";
        }
        return false;
    }

    try {
        auto this_audio = getAudioMedia(this_idx);
        auto other_audio = other->getAudioMedia(other_idx);
        this_audio.stopTransmit(other_audio);
        other_audio.stopTransmit(this_audio);

        // [P2-2 Fix] 언브릿지 시 AI 엔진으로의 음성 입력 재개
        if (media_port_)
            media_port_->setAiPaused(false);
        if (other->media_port_)
            other->media_port_->setAiPaused(false);

        spdlog::info("[Call] Session={} unbridged from Session={}", session_id_,
                     other->session_id_);
        return true;
    } catch (const pj::Error& e) {
        if (error_message) {
            *error_message = e.info();
        }
        return false;
    }
}

// [R-3 Fix] Graceful Shutdown 시 AI 세션 명시적 종료
// SessionManager::endAllAiSessions()에서 호출되어
// hangup→onCallState(DISCONNECTED) 체인이 타임아웃 내에 완료되지 않을 경우에도
// gRPC 스트림이 orphan 상태로 남지 않도록 보장
void VoicebotCall::endAiSession()
{
    std::lock_guard<std::mutex> lock(ai_init_mutex_);
    if (ai_client_) {
        spdlog::info("[Call] Session={} Ending AI session explicitly", session_id_);
        ai_client_->endSession();
        ai_client_.reset();
    }
}

void VoicebotCall::dumpCdr(const std::string& reason)
{
    auto end_time = std::chrono::system_clock::now();
    auto duration =
        std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time_).count();

    // 타임스탬프 포맷팅
    std::time_t end_time_t = std::chrono::system_clock::to_time_t(end_time);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&end_time_t));

    // [E-1 Fix] 운영 및 과금 목적의 CDR 구조화 로깅
    spdlog::info("[CDR] {{\"session_id\":\"{}\", "
                 "\"end_time\":\"{}\", "
                 "\"duration_sec\":{}, \"reason\":\"{}\", "
                 "\"vad_triggers\":{}, \"bargeins\":{}}}",
                 session_id_.empty() ? "N/A" : session_id_, buf, duration,
                 reason.empty() ? "Unknown" : reason, vad_trigger_count_.load(),
                 bargein_count_.load());
}
