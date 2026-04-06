#pragma once
#include <grpcpp/grpcpp.h>
#include <grpcpp/security/credentials.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

// [CR-1 + H-6 Fix] 환경변수를 프로세스 시작 시 1회 읽어 캐싱하는 싱글톤
// getenv()는 스레드-세이프하지 않은 구현이 존재하며, 매 콜 경로에서 반복 호출 방지
// gRPC 채널도 싱글톤으로 관리하여 매 콜마다 TCP/TLS 핸드셰이크 비용 제거
class AppConfig
{
public:
    static const AppConfig& instance()
    {
        static AppConfig config;
        return config;
    }

    // ── SIP 설정 ──
    int sip_port;
    bool sip_use_tls;
    bool srtp_enable;
    std::string sip_tls_cert_file;
    std::string sip_tls_privkey_file;
    std::string sip_tls_ca_file;

    // ── PBX 설정 ──
    std::string pbx_uri;
    std::string pbx_id_uri;
    std::string pbx_username;
    std::string pbx_password;
    bool pbx_mode;  // 4개 모두 설정되었는지

    // ── AI 엔진 (gRPC) 설정 ──
    std::string ai_engine_addr;
    bool grpc_use_tls;
    std::string grpc_tls_ca_cert;
    std::string grpc_tls_client_cert;
    std::string grpc_tls_client_key;
    int grpc_stream_deadline_secs;
    int grpc_max_reconnect_retries;
    int grpc_max_backoff_ms;

    // ── VAD 설정 ──
    std::string silero_vad_model_path;

    // ── 세션/채널 설정 ──
    int max_concurrent_calls;
    int answer_delay_ms;
    int tts_buffer_secs;

    // ── RTP 포트 범위 ──
    int rtp_port_min;
    int rtp_port_max;

    // ── 모니터링 / API 서버 ──
    int http_port;
    std::string admin_api_key;
    int admin_api_rate_limit_rps;
    int admin_api_rate_limit_burst;
    int admin_api_max_body_bytes;
    int admin_api_max_header_bytes;

    // ── 로깅 설정 ──
    std::string log_level;
    std::string log_dir;
    int pjsip_log_level;
    bool pjsip_null_audio;

    // ── 런타임 프로파일 ──
    std::string runtime_profile;

    // [CR-1 Fix] gRPC 채널을 싱글톤으로 공유
    // TCP 연결 + HTTP/2 핸드셰이크 + TLS 협상을 1회만 수행
    std::shared_ptr<grpc::Channel> getGrpcChannel() const
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        if (!grpc_channel_) {
            grpc_channel_ = createGrpcChannel();
        }
        return grpc_channel_;
    }

    bool isProductionProfile() const
    {
        auto profile = runtime_profile;
        std::transform(profile.begin(), profile.end(), profile.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return profile == "prod" || profile == "production";
    }

    bool validateRuntimeSecurityPolicy(std::vector<std::string>* errors = nullptr) const
    {
        std::vector<std::string> local_errors;

        if (isProductionProfile()) {
            if (!sip_use_tls) {
                local_errors.emplace_back("SIP_USE_TLS must be enabled in production profile.");
            }
            if (!grpc_use_tls) {
                local_errors.emplace_back("GRPC_USE_TLS must be enabled in production profile.");
            }
            if (!srtp_enable) {
                local_errors.emplace_back("SRTP_ENABLE must be enabled in production profile.");
            }

            validateFileForProd(sip_tls_cert_file, "SIP_TLS_CERT_FILE", local_errors);
            validateFileForProd(sip_tls_privkey_file, "SIP_TLS_PRIVKEY_FILE", local_errors);
            validateFileForProd(sip_tls_ca_file, "SIP_TLS_CA_FILE", local_errors);
            validateFileForProd(grpc_tls_ca_cert, "GRPC_TLS_CA_CERT", local_errors);
            validateFileForProd(grpc_tls_client_cert, "GRPC_TLS_CLIENT_CERT", local_errors);
            validateFileForProd(grpc_tls_client_key, "GRPC_TLS_CLIENT_KEY", local_errors);

            if (!isStrongAdminApiKey(admin_api_key)) {
                local_errors.emplace_back(
                    "ADMIN_API_KEY must be strong (>=16 chars, upper/lower/digit/symbol, "
                    "non-default) in production profile.");
            }

            if (pjsip_null_audio) {
                local_errors.emplace_back(
                    "PJSIP_NULL_AUDIO must be disabled in production profile.");
            }

            validateIntRangeForProd(admin_api_rate_limit_rps, 1, 10000, "ADMIN_API_RATE_LIMIT_RPS",
                                    local_errors);
            validateIntRangeForProd(admin_api_rate_limit_burst, 1, 100000,
                                    "ADMIN_API_RATE_LIMIT_BURST", local_errors);
            validateIntRangeForProd(admin_api_max_body_bytes, 256, 1024 * 1024,
                                    "ADMIN_API_MAX_BODY_BYTES", local_errors);
            validateIntRangeForProd(admin_api_max_header_bytes, 1024, 256 * 1024,
                                    "ADMIN_API_MAX_HEADER_BYTES", local_errors);

            if (admin_api_rate_limit_burst < admin_api_rate_limit_rps) {
                local_errors.emplace_back("ADMIN_API_RATE_LIMIT_BURST must be >= "
                                          "ADMIN_API_RATE_LIMIT_RPS in production profile.");
            }

            if (sip_port == http_port) {
                local_errors.emplace_back(
                    "SIP_PORT and HTTP_PORT must not be the same in production profile.");
            }
        }

        if (errors) {
            errors->insert(errors->end(), local_errors.begin(), local_errors.end());
        }
        return local_errors.empty();
    }

private:
    mutable std::mutex channel_mutex_;
    mutable std::shared_ptr<grpc::Channel> grpc_channel_;

    AppConfig()
    {
        // ── SIP ──
        sip_port = readInt("SIP_PORT", 5060, 1, 65535);
        sip_use_tls = readBool("SIP_USE_TLS", false);
        srtp_enable = readBool("SRTP_ENABLE", false);
        sip_tls_cert_file = readStr("SIP_TLS_CERT_FILE", "");
        sip_tls_privkey_file = readStr("SIP_TLS_PRIVKEY_FILE", "");
        sip_tls_ca_file = readStr("SIP_TLS_CA_FILE", "");

        // ── PBX ──
        pbx_uri = readStr("PBX_URI", "");
        pbx_id_uri = readStr("PBX_ID_URI", "");
        pbx_username = readStr("PBX_USERNAME", "");
        pbx_password = readStr("PBX_PASSWORD", "");
        pbx_mode = !pbx_uri.empty() && !pbx_id_uri.empty() && !pbx_username.empty() &&
                   !pbx_password.empty();

        // ── AI Engine (gRPC) ──
        ai_engine_addr = readStr("AI_ENGINE_ADDR", "localhost:50051");
        grpc_use_tls = readBool("GRPC_USE_TLS", false);
        grpc_tls_ca_cert = readStr("GRPC_TLS_CA_CERT", "");
        grpc_tls_client_cert = readStr("GRPC_TLS_CLIENT_CERT", "");
        grpc_tls_client_key = readStr("GRPC_TLS_CLIENT_KEY", "");
        grpc_stream_deadline_secs = readInt("GRPC_STREAM_DEADLINE_SECS", 86400, 1, 86400);
        grpc_max_reconnect_retries = readInt("GRPC_MAX_RECONNECT_RETRIES", 5, 1, 100);
        grpc_max_backoff_ms = readInt("GRPC_MAX_BACKOFF_MS", 4000, 500, 60000);

        // ── AI Engine address validation ──
        validateAiAddr();

        // ── VAD ──
        silero_vad_model_path = readStr("SILERO_VAD_MODEL_PATH", "models/silero_vad.onnx");

        // ── Session ──
        max_concurrent_calls = readInt("MAX_CONCURRENT_CALLS", 100, 1, 10000);
        answer_delay_ms = readInt("ANSWER_DELAY_MS", 200, 0, 5000);
        tts_buffer_secs = readInt("TTS_BUFFER_SECS", 5, 1, 60);

        // ── RTP Port Range ──
        rtp_port_min = readInt("RTP_PORT_MIN", 16000, 1024, 65535);
        rtp_port_max = readInt("RTP_PORT_MAX", 16100, 1024, 65535);
        if (rtp_port_max < rtp_port_min) {
            spdlog::warn("[Config] RTP_PORT_MAX({}) < RTP_PORT_MIN({}) — swapping", rtp_port_max,
                         rtp_port_min);
            std::swap(rtp_port_min, rtp_port_max);
        }

        // ── Logging ──
        log_level = readStr("LOG_LEVEL", "info");
        log_dir = readStr("LOG_DIR", "");
        pjsip_log_level = readInt("PJSIP_LOG_LEVEL", 3, 0, 6);
        pjsip_null_audio = readBool("PJSIP_NULL_AUDIO", false);

        // ── API Server ──
        http_port = readInt("HTTP_PORT", 8080, 1, 65535);
        admin_api_key = readStr("ADMIN_API_KEY", "changeme-admin-key");
        admin_api_rate_limit_rps = readInt("ADMIN_API_RATE_LIMIT_RPS", 20, 1, 10000);
        admin_api_rate_limit_burst = readInt("ADMIN_API_RATE_LIMIT_BURST", 40, 1, 100000);
        admin_api_max_body_bytes = readInt("ADMIN_API_MAX_BODY_BYTES", 8192, 256, 1024 * 1024);
        admin_api_max_header_bytes = readInt("ADMIN_API_MAX_HEADER_BYTES", 16384, 1024, 256 * 1024);

        // ── Runtime Profile ──
        runtime_profile = readStr("VBGW_PROFILE", "dev");
    }

    ~AppConfig() = default;
    AppConfig(const AppConfig&) = delete;
    AppConfig& operator=(const AppConfig&) = delete;

    // ── Helper: 환경변수 읽기 유틸리티 ──

    static std::string readStr(const char* name, const std::string& def)
    {
        const char* val = std::getenv(name);
        return (val && *val) ? std::string(val) : def;
    }

    static int readInt(const char* name, int def, int min_val, int max_val)
    {
        const char* val = std::getenv(name);
        if (!val || !*val)
            return def;
        try {
            int v = std::stoi(val);
            if (v < min_val || v > max_val) {
                spdlog::warn("[Config] {}={} out of range [{},{}] — using {}", name, v, min_val,
                             max_val, def);
                return def;
            }
            return v;
        } catch (...) {
            spdlog::warn("[Config] {}='{}' is not a valid integer — using {}", name, val, def);
            return def;
        }
    }

    static bool readBool(const char* name, bool def)
    {
        const char* val = std::getenv(name);
        if (!val || !*val)
            return def;
        std::string normalized(val);
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return normalized == "1" || normalized == "true" || normalized == "yes" ||
               normalized == "on";
    }

    void validateAiAddr() const
    {
        auto colon = ai_engine_addr.rfind(':');
        bool valid = (colon != std::string::npos && colon > 0 && colon + 1 < ai_engine_addr.size());
        if (valid) {
            const std::string port_str = ai_engine_addr.substr(colon + 1);
            for (char c : port_str) {
                if (!std::isdigit(static_cast<unsigned char>(c))) {
                    valid = false;
                    break;
                }
            }
            if (valid) {
                int port = std::stoi(port_str);
                if (port < 1 || port > 65535)
                    valid = false;
            }
        }
        if (!valid) {
            spdlog::critical("[Config] Invalid AI_ENGINE_ADDR='{}' — expected host:port",
                             ai_engine_addr);
        }
    }

    // [CR-1 Fix] gRPC 채널 생성 (1회만 호출됨)
    std::shared_ptr<grpc::Channel> createGrpcChannel() const
    {
        std::shared_ptr<grpc::ChannelCredentials> creds;

        if (grpc_use_tls) {
            try {
                grpc::SslCredentialsOptions ssl_opts;
                ssl_opts.pem_root_certs = readPemFile(grpc_tls_ca_cert, "GRPC_TLS_CA_CERT");
                ssl_opts.pem_private_key = readPemFile(grpc_tls_client_key, "GRPC_TLS_CLIENT_KEY");
                ssl_opts.pem_cert_chain = readPemFile(grpc_tls_client_cert, "GRPC_TLS_CLIENT_CERT");
                creds = grpc::SslCredentials(ssl_opts);
                spdlog::info("[Config] gRPC TLS/mTLS channel created");
            } catch (const std::runtime_error& e) {
                spdlog::critical("[Config] gRPC TLS setup failed: {}", e.what());
                // TLS 실패 시 insecure 폴백하지 않음 — 보안 정책
                throw;
            }
        } else {
            creds = grpc::InsecureChannelCredentials();
            spdlog::warn(
                "[Config] gRPC using INSECURE channel — set GRPC_USE_TLS=1 for production");
        }

        return grpc::CreateChannel(ai_engine_addr, creds);
    }

    static std::string readPemFile(const std::string& path, const char* env_name)
    {
        if (path.empty()) {
            throw std::runtime_error(std::string("TLS cert path not set: ") + env_name);
        }
        std::ifstream f(path);
        if (!f.is_open()) {
            throw std::runtime_error(std::string("TLS cert file not found: ") + path);
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    static void validateFileForProd(const std::string& path, const char* name,
                                    std::vector<std::string>& errors)
    {
        if (path.empty()) {
            errors.emplace_back(std::string(name) + " is required in production profile.");
            return;
        }
        if (!std::filesystem::exists(path)) {
            errors.emplace_back(std::string(name) + " path does not exist: " + path);
        }
    }

    static bool isStrongAdminApiKey(const std::string& key)
    {
        if (key.empty() || key == "changeme-admin-key" || key.size() < 16) {
            return false;
        }

        bool has_upper = false;
        bool has_lower = false;
        bool has_digit = false;
        bool has_symbol = false;

        for (unsigned char c : key) {
            if (std::isupper(c)) {
                has_upper = true;
            } else if (std::islower(c)) {
                has_lower = true;
            } else if (std::isdigit(c)) {
                has_digit = true;
            } else {
                has_symbol = true;
            }
        }
        return has_upper && has_lower && has_digit && has_symbol;
    }

    static void validateIntRangeForProd(int value, int min_val, int max_val, const char* name,
                                        std::vector<std::string>& errors)
    {
        if (value < min_val || value > max_val) {
            std::ostringstream msg;
            msg << name << " must be in range [" << min_val << "," << max_val
                << "] in production profile.";
            errors.emplace_back(msg.str());
        }
    }
};
