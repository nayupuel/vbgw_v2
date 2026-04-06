#include "HttpServer.h"

#include "../engine/SessionManager.h"
#include "../engine/VoicebotAccount.h"
#include "../utils/AppConfig.h"
#include "../utils/RuntimeMetrics.h"

#include <boost/asio.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <sstream>
#include <unordered_map>
#include <vector>

using boost::asio::ip::tcp;

namespace {

std::string trim(std::string s)
{
    auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    s.erase(s.begin(),
            std::find_if(s.begin(), s.end(), [&](unsigned char c) { return !is_space(c); }));
    s.erase(
        std::find_if(s.rbegin(), s.rend(), [&](unsigned char c) { return !is_space(c); }).base(),
        s.end());
    return s;
}

std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string boolToJson(bool value)
{
    return value ? "true" : "false";
}

bool constantTimeEquals(const std::string& lhs, const std::string& rhs)
{
    const std::size_t max_len = std::max(lhs.size(), rhs.size());
    unsigned char diff = static_cast<unsigned char>(lhs.size() ^ rhs.size());
    for (std::size_t i = 0; i < max_len; ++i) {
        const unsigned char a = i < lhs.size() ? static_cast<unsigned char>(lhs[i]) : 0;
        const unsigned char b = i < rhs.size() ? static_cast<unsigned char>(rhs[i]) : 0;
        diff = static_cast<unsigned char>(diff | (a ^ b));
    }
    return diff == 0;
}

bool isGatewayReady(const RuntimeMetrics& metrics)
{
    const bool sip_ok = !metrics.sipPbxMode() || metrics.sipRegistered();
    const bool grpc_ok = (metrics.grpcActiveSessions() == 0) || metrics.grpcHealthy();
    return sip_ok && grpc_ok;
}

std::string jsonEscape(const std::string& input)
{
    std::string out;
    out.reserve(input.size());
    for (char c : input) {
        if (c == '\\' || c == '"') {
            out.push_back('\\');
            out.push_back(c);
        } else if (c == '\n') {
            out.append("\\n");
        } else if (c == '\r') {
            out.append("\\r");
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// [P-4 Fix] std::regex 제거 — 수동 파싱으로 대체 (더 빠르고 의존성 없음)
bool parseTargetUri(const std::string& body, std::string* target_uri)
{
    const std::string key = "\"target_uri\"";
    auto key_pos = body.find(key);
    if (key_pos == std::string::npos) {
        return false;
    }
    // key 뒤의 ':' 찾기
    auto colon_pos = body.find(':', key_pos + key.size());
    if (colon_pos == std::string::npos) {
        return false;
    }
    // 값의 시작 '"' 찾기
    auto quote_start = body.find('"', colon_pos + 1);
    if (quote_start == std::string::npos) {
        return false;
    }
    // 값의 끝 '"' 찾기
    auto quote_end = body.find('"', quote_start + 1);
    if (quote_end == std::string::npos || quote_end <= quote_start + 1) {
        return false;
    }
    *target_uri = body.substr(quote_start + 1, quote_end - quote_start - 1);
    return true;
}

// [S-2 Fix] SIP URI 인젝션 방어 강화
// CRLF, 세미콜론, 꾸인괄호 등 SIP 헤더 인젝션에 사용될 수 있는 문자 차단
bool isValidSipUri(const std::string& target_uri)
{
    if (target_uri.size() < 8 || target_uri.size() > 256) {
        return false;
    }
    const bool has_valid_scheme =
        target_uri.rfind("sip:", 0) == 0 || target_uri.rfind("sips:", 0) == 0;
    if (!has_valid_scheme) {
        return false;
    }
    if (target_uri.find('@') == std::string::npos) {
        return false;
    }
    // [S-2 Fix] SIP 헤더 인젝션 문자 차단 (CWE-113)
    if (target_uri.find_first_of("\r\n\t <>;\"\'`|&") != std::string::npos) {
        return false;
    }
    return true;
}

}  // namespace

HttpServer::HttpServer() {}

HttpServer::~HttpServer()
{
    stop();
}

bool HttpServer::start(int port)
{
    if (is_running_.exchange(true, std::memory_order_acq_rel)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(rate_limit_mutex_);
        rate_limit_initialized_ = false;
        rate_limit_tokens_ = 0.0;
    }
    listen_port_.store(port, std::memory_order_release);
    server_thread_ = std::make_unique<std::thread>([this, port]() { runServer(port); });
    spdlog::info("[HttpServer] HTTP Admin Server starting on port {}", port);
    return true;
}

void HttpServer::setAccount(VoicebotAccount* account)
{
    std::lock_guard<std::mutex> lock(account_mutex_);
    account_ = account;
}

void HttpServer::stop()
{
    if (!is_running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    const int port = listen_port_.load(std::memory_order_acquire);
    if (port > 0) {
        try {
            boost::asio::io_context io_context;
            tcp::socket wake_socket(io_context);
            boost::system::error_code ec;
            wake_socket.connect(tcp::endpoint(boost::asio::ip::address_v4::loopback(),
                                              static_cast<unsigned short>(port)),
                                ec);
            wake_socket.close();
        } catch (...) {}
    }

    if (server_thread_ && server_thread_->joinable()) {
        server_thread_->join();
    }
    server_thread_.reset();

    // [A-1 Fix] 워커 스레드 정리
    {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        for (auto& t : worker_threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
        worker_threads_.clear();
    }

    listen_port_.store(0, std::memory_order_release);
    spdlog::info("[HttpServer] HTTP Admin Server stopped");
}

void HttpServer::runServer(int port)
{
    try {
        boost::asio::io_context io_context;
        tcp::acceptor acceptor(io_context);
        acceptor.open(tcp::v4());
        acceptor.set_option(tcp::acceptor::reuse_address(true));
        acceptor.bind(tcp::endpoint(tcp::v4(), static_cast<unsigned short>(port)));
        acceptor.listen();

        while (is_running_.load(std::memory_order_acquire)) {
            auto socket = std::make_shared<tcp::socket>(io_context);
            boost::system::error_code accept_error;
            acceptor.accept(*socket, accept_error);
            if (accept_error) {
                if (!is_running_.load(std::memory_order_acquire)) {
                    break;
                }
                spdlog::warn("[HttpServer] accept() failed: {}", accept_error.message());
                continue;
            }

            // [A-1 Fix] 연결당 스레드 디스패치 — health/metrics 프로브와 outbound API 비간섭
            // K8s readiness probe가 outbound call 처리 지연에 의해 블로킹되는 것을 방지
            {
                std::lock_guard<std::mutex> lock(workers_mutex_);

                // 완료된 워커 정리
                worker_threads_.erase(std::remove_if(worker_threads_.begin(), worker_threads_.end(),
                                                     [](std::thread& t) {
                                                         if (t.joinable()) {
                                                             // 간이 확인: native_handle로 완료 여부
                                                             // 판단 불가 안전하게 try_join 대안으로
                                                             // 항상 유지
                                                             return false;
                                                         }
                                                         return true;
                                                     }),
                                      worker_threads_.end());

                if (static_cast<int>(worker_threads_.size()) >= kMaxWorkerThreads) {
                    // 워커 풀 초과 시 가장 오래된 워커 join 후 교체
                    if (worker_threads_.front().joinable()) {
                        worker_threads_.front().join();
                    }
                    worker_threads_.erase(worker_threads_.begin());
                }

                worker_threads_.emplace_back(
                    [this, socket]() { handleConnectionImpl(static_cast<void*>(&(*socket))); });
            }
        }
    } catch (const std::exception& e) {
        spdlog::error("[HttpServer] Server error: {}", e.what());
    }
}

// [A-1 Fix] 개별 연결 처리 — 워커 스레드에서 실행
void HttpServer::handleConnectionImpl(void* socket_ptr)
{
    auto* socket = static_cast<tcp::socket*>(socket_ptr);
    try {
        const auto& cfg = AppConfig::instance();

        std::string remote_ip = "unknown";
        boost::system::error_code ep_error;
        auto remote_ep = socket->remote_endpoint(ep_error);
        if (!ep_error) {
            remote_ip = remote_ep.address().to_string();
        }

        boost::asio::streambuf request(static_cast<std::size_t>(cfg.admin_api_max_header_bytes));
        boost::system::error_code read_error;
        boost::asio::read_until(*socket, request, "\r\n\r\n", read_error);

        if (read_error == boost::asio::error::not_found) {
            const auto response =
                makeHttpResponse(431, "Request Header Fields Too Large",
                                 "{\"error\":\"header_too_large\"}", "application/json");
            boost::system::error_code write_error;
            boost::asio::write(*socket, boost::asio::buffer(response), write_error);
            return;
        }

        if (read_error && read_error != boost::asio::error::eof) {
            spdlog::warn("[HttpServer] Failed to read request header: {}", read_error.message());
            return;
        }

        std::istream request_stream(&request);
        std::string request_line;
        std::getline(request_stream, request_line);
        if (!request_line.empty() && request_line.back() == '\r') {
            request_line.pop_back();
        }

        std::string req_method;
        std::string req_path;
        std::string req_version;
        std::istringstream req_line_stream(request_line);
        req_line_stream >> req_method >> req_path >> req_version;
        if (req_method.empty() || req_path.empty() || req_version.empty()) {
            const auto response = makeHttpResponse(
                400, "Bad Request", "{\"error\":\"invalid_request_line\"}", "application/json");
            boost::system::error_code write_error;
            boost::asio::write(*socket, boost::asio::buffer(response), write_error);
            return;
        }

        std::unordered_map<std::string, std::string> headers;
        int content_length = 0;
        std::string header_line;
        while (std::getline(request_stream, header_line) && header_line != "\r") {
            if (!header_line.empty() && header_line.back() == '\r') {
                header_line.pop_back();
            }

            auto colon_pos = header_line.find(':');
            if (colon_pos == std::string::npos) {
                continue;
            }

            std::string key = toLower(trim(header_line.substr(0, colon_pos)));
            std::string value = trim(header_line.substr(colon_pos + 1));
            headers[key] = value;

            if (key == "content-length") {
                try {
                    content_length = std::max(0, std::stoi(value));
                } catch (...) {
                    content_length = 0;
                }
            }
        }

        if (content_length > cfg.admin_api_max_body_bytes) {
            const auto response =
                makeHttpResponse(413, "Payload Too Large",
                                 "{\"error\":\"payload_too_large\",\"message\":\"request body "
                                 "exceeds ADMIN_API_MAX_BODY_BYTES\"}",
                                 "application/json");
            boost::system::error_code write_error;
            boost::asio::write(*socket, boost::asio::buffer(response), write_error);
            return;
        }

        std::string body;
        if (content_length > 0) {
            std::size_t already_buffered = request.size();
            std::size_t to_read_from_buffer =
                std::min<std::size_t>(already_buffered, static_cast<std::size_t>(content_length));
            if (to_read_from_buffer > 0) {
                body.resize(to_read_from_buffer);
                request_stream.read(&body[0], static_cast<std::streamsize>(to_read_from_buffer));
                content_length -= static_cast<int>(to_read_from_buffer);
            }
            if (content_length > 0) {
                std::vector<char> body_buf(static_cast<std::size_t>(content_length));
                boost::asio::read(*socket, boost::asio::buffer(body_buf), read_error);
                if (!read_error || read_error == boost::asio::error::eof) {
                    body.append(body_buf.begin(), body_buf.end());
                }
            }
        }

        std::string response;
        if (req_method == "GET" && req_path == "/live") {
            response = handleLiveness();
        } else if (req_method == "GET" && req_path == "/ready") {
            response = handleReadiness();
        } else if (req_method == "GET" && req_path == "/health") {
            response = handleHealthCheck();
        } else if (req_method == "GET" && req_path == "/metrics") {
            response = handleMetrics();
        } else if (req_method == "POST" && req_path == "/api/v1/calls") {
            const auto it = headers.find("x-admin-key");
            const std::string admin_key = (it != headers.end()) ? it->second : "";
            response = handleOutboundCall(body, admin_key, remote_ip);
        } else {
            response =
                makeHttpResponse(404, "Not Found", "{\"error\":\"not_found\"}", "application/json");
        }

        boost::system::error_code write_error;
        boost::asio::write(*socket, boost::asio::buffer(response), write_error);
        if (write_error) {
            spdlog::warn("[HttpServer] write() failed: {}", write_error.message());
        }
        socket->close();
    } catch (const std::exception& e) {
        spdlog::warn("[HttpServer] Connection handler error: {}", e.what());
    } catch (...) {
        spdlog::warn("[HttpServer] Connection handler unknown error");
    }
}

std::string HttpServer::handleHealthCheck() const
{
    const auto& cfg = AppConfig::instance();
    auto& metrics = RuntimeMetrics::instance();
    const auto active_calls = SessionManager::getInstance().getActiveCallCount();
    const bool overall_ok = isGatewayReady(metrics);

    std::ostringstream body;
    body << "{\"status\":\"" << (overall_ok ? "UP" : "DEGRADED") << "\","
         << "\"profile\":\"" << jsonEscape(cfg.runtime_profile) << "\","
         << "\"active_calls\":" << active_calls << ","
         << "\"sip\":{\"mode\":\"" << (metrics.sipPbxMode() ? "PBX" : "LOCAL")
         << "\",\"registered\":" << boolToJson(metrics.sipRegistered())
         << ",\"last_status_code\":" << metrics.sipLastStatusCode() << "},"
         << "\"grpc\":{\"healthy\":" << boolToJson(metrics.grpcHealthy())
         << ",\"active_sessions\":" << metrics.grpcActiveSessions()
         << ",\"queued_frames\":" << metrics.grpcQueuedFrames()
         << ",\"dropped_frames_total\":" << metrics.grpcDroppedFramesTotal()
         << ",\"reconnect_attempts_total\":" << metrics.grpcReconnectAttemptsTotal()
         << ",\"stream_errors_total\":" << metrics.grpcStreamErrorsTotal() << "},"
         << "\"admin_api\":{\"outbound_requests_total\":" << metrics.adminApiOutboundRequestsTotal()
         << ",\"outbound_accepted_total\":" << metrics.adminApiOutboundAcceptedTotal()
         << ",\"outbound_rejected_auth_total\":" << metrics.adminApiOutboundRejectedAuthTotal()
         << ",\"outbound_rejected_rate_limited_total\":"
         << metrics.adminApiOutboundRejectedRateLimitedTotal()
         << ",\"outbound_rejected_invalid_total\":"
         << metrics.adminApiOutboundRejectedInvalidTotal()
         << ",\"outbound_failed_total\":" << metrics.adminApiOutboundFailedTotal() << "}}";

    return makeHttpResponse(200, "OK", body.str(), "application/json");
}

std::string HttpServer::handleLiveness() const
{
    return makeHttpResponse(200, "OK", "{\"status\":\"UP\"}", "application/json");
}

std::string HttpServer::handleReadiness() const
{
    auto& metrics = RuntimeMetrics::instance();
    const bool ready = isGatewayReady(metrics);

    std::ostringstream body;
    body << "{\"status\":\"" << (ready ? "READY" : "NOT_READY") << "\","
         << "\"sip_registered\":" << boolToJson(metrics.sipRegistered()) << ","
         << "\"grpc_healthy\":" << boolToJson(metrics.grpcHealthy()) << "}";

    return makeHttpResponse(ready ? 200 : 503, ready ? "OK" : "Service Unavailable", body.str(),
                            "application/json");
}

std::string HttpServer::handleMetrics() const
{
    auto& metrics = RuntimeMetrics::instance();
    const auto active_calls = SessionManager::getInstance().getActiveCallCount();

    std::ostringstream body;
    body
        << "# HELP vbgw_active_calls Number of active voice calls\n"
        << "# TYPE vbgw_active_calls gauge\n"
        << "vbgw_active_calls " << active_calls << "\n"
        << "# HELP vbgw_sip_registered SIP registration state (1=registered)\n"
        << "# TYPE vbgw_sip_registered gauge\n"
        << "vbgw_sip_registered " << (metrics.sipRegistered() ? 1 : 0) << "\n"
        << "# HELP vbgw_grpc_active_sessions Active gRPC stream sessions\n"
        << "# TYPE vbgw_grpc_active_sessions gauge\n"
        << "vbgw_grpc_active_sessions " << metrics.grpcActiveSessions() << "\n"
        << "# HELP vbgw_grpc_queued_frames Number of queued audio frames for gRPC\n"
        << "# TYPE vbgw_grpc_queued_frames gauge\n"
        << "vbgw_grpc_queued_frames " << metrics.grpcQueuedFrames() << "\n"
        << "# HELP vbgw_grpc_dropped_frames_total Dropped audio frames due to backpressure\n"
        << "# TYPE vbgw_grpc_dropped_frames_total counter\n"
        << "vbgw_grpc_dropped_frames_total " << metrics.grpcDroppedFramesTotal() << "\n"
        << "# HELP vbgw_grpc_reconnect_attempts_total gRPC reconnect attempts\n"
        << "# TYPE vbgw_grpc_reconnect_attempts_total counter\n"
        << "vbgw_grpc_reconnect_attempts_total " << metrics.grpcReconnectAttemptsTotal() << "\n"
        << "# HELP vbgw_grpc_stream_errors_total gRPC stream errors\n"
        << "# TYPE vbgw_grpc_stream_errors_total counter\n"
        << "vbgw_grpc_stream_errors_total " << metrics.grpcStreamErrorsTotal() << "\n"
        << "# HELP vbgw_vad_speech_events_total VAD speech-start edge events\n"
        << "# TYPE vbgw_vad_speech_events_total counter\n"
        << "vbgw_vad_speech_events_total " << metrics.vadSpeechEventsTotal() << "\n"
        << "# HELP vbgw_barge_in_events_total Barge-in events observed from AI engine\n"
        << "# TYPE vbgw_barge_in_events_total counter\n"
        << "vbgw_barge_in_events_total " << metrics.bargeInEventsTotal() << "\n"
        << "# HELP vbgw_admin_api_outbound_requests_total Total outbound call API requests\n"
        << "# TYPE vbgw_admin_api_outbound_requests_total counter\n"
        << "vbgw_admin_api_outbound_requests_total " << metrics.adminApiOutboundRequestsTotal()
        << "\n"
        << "# HELP vbgw_admin_api_outbound_accepted_total Accepted outbound call requests\n"
        << "# TYPE vbgw_admin_api_outbound_accepted_total counter\n"
        << "vbgw_admin_api_outbound_accepted_total " << metrics.adminApiOutboundAcceptedTotal()
        << "\n"
        << "# HELP vbgw_admin_api_outbound_rejected_auth_total Outbound requests rejected by auth\n"
        << "# TYPE vbgw_admin_api_outbound_rejected_auth_total counter\n"
        << "vbgw_admin_api_outbound_rejected_auth_total "
        << metrics.adminApiOutboundRejectedAuthTotal() << "\n"
        << "# HELP vbgw_admin_api_outbound_rejected_rate_limited_total Outbound requests "
           "rate-limited\n"
        << "# TYPE vbgw_admin_api_outbound_rejected_rate_limited_total counter\n"
        << "vbgw_admin_api_outbound_rejected_rate_limited_total "
        << metrics.adminApiOutboundRejectedRateLimitedTotal() << "\n"
        << "# HELP vbgw_admin_api_outbound_rejected_invalid_total Outbound requests rejected by "
           "validation\n"
        << "# TYPE vbgw_admin_api_outbound_rejected_invalid_total counter\n"
        << "vbgw_admin_api_outbound_rejected_invalid_total "
        << metrics.adminApiOutboundRejectedInvalidTotal() << "\n"
        << "# HELP vbgw_admin_api_outbound_failed_total Outbound requests failed after validation\n"
        << "# TYPE vbgw_admin_api_outbound_failed_total counter\n"
        << "vbgw_admin_api_outbound_failed_total " << metrics.adminApiOutboundFailedTotal() << "\n";

    return makeHttpResponse(200, "OK", body.str(), "text/plain; version=0.0.4");
}

std::string HttpServer::handleOutboundCall(const std::string& request_body,
                                           const std::string& admin_key,
                                           const std::string& remote_ip) const
{
    const auto& cfg = AppConfig::instance();
    auto& metrics = RuntimeMetrics::instance();
    metrics.incAdminApiOutboundRequests();

    if (cfg.admin_api_key.empty()) {
        metrics.incAdminApiOutboundFailed();
        spdlog::error(
            "[Audit][HTTP] outbound-call rejected: ADMIN_API_KEY is not configured [remote_ip={}]",
            remote_ip);
        return makeHttpResponse(503, "Service Unavailable",
                                "{\"error\":\"admin_api_key_not_configured\"}", "application/json");
    }

    if (admin_key.empty()) {
        metrics.incAdminApiOutboundRejectedAuth();
        spdlog::warn("[Audit][HTTP] outbound-call rejected: missing X-Admin-Key [remote_ip={}]",
                     remote_ip);
        return makeHttpResponse(401, "Unauthorized", "{\"error\":\"missing_admin_key\"}",
                                "application/json");
    }

    if (!constantTimeEquals(admin_key, cfg.admin_api_key)) {
        metrics.incAdminApiOutboundRejectedAuth();
        spdlog::warn("[Audit][HTTP] outbound-call rejected: invalid X-Admin-Key [remote_ip={}]",
                     remote_ip);
        return makeHttpResponse(403, "Forbidden", "{\"error\":\"invalid_admin_key\"}",
                                "application/json");
    }

    double retry_after_sec = 0.0;
    if (!consumeOutboundRateLimitToken(&retry_after_sec)) {
        metrics.incAdminApiOutboundRejectedRateLimited();
        const auto retry_after_i = std::max(1, static_cast<int>(std::ceil(retry_after_sec)));
        spdlog::warn("[Audit][HTTP] outbound-call rate-limited [remote_ip={}, retry_after={}s]",
                     remote_ip, retry_after_i);
        return makeHttpResponse(
            429, "Too Many Requests",
            "{\"error\":\"rate_limited\",\"message\":\"outbound call API rate limit exceeded\"}",
            "application/json", {{"Retry-After", std::to_string(retry_after_i)}});
    }

    std::string target_uri;
    if (!parseTargetUri(request_body, &target_uri) || !isValidSipUri(target_uri)) {
        metrics.incAdminApiOutboundRejectedInvalid();
        spdlog::warn(
            "[Audit][HTTP] outbound-call rejected: invalid request body [remote_ip={}, body={}]",
            remote_ip, request_body);
        return makeHttpResponse(400, "Bad Request",
                                "{\"error\":\"invalid_request\",\"message\":\"target_uri must be a "
                                "valid sip/sips URI\"}",
                                "application/json");
    }

    VoicebotAccount* account = nullptr;
    {
        std::lock_guard<std::mutex> lock(account_mutex_);
        account = account_;
    }
    if (!account) {
        metrics.incAdminApiOutboundFailed();
        spdlog::error("[Audit][HTTP] outbound-call rejected: account not ready [remote_ip={}, "
                      "target_uri={}]",
                      remote_ip, target_uri);
        return makeHttpResponse(503, "Service Unavailable",
                                "{\"error\":\"voicebot_account_not_ready\"}", "application/json");
    }

    int call_id = PJSUA_INVALID_ID;
    std::string call_error;
    if (!account->makeOutboundCall(target_uri, &call_id, &call_error)) {
        const bool capacity_error = call_error.find("Maximum concurrent") != std::string::npos;
        if (capacity_error) {
            metrics.incAdminApiOutboundRejectedRateLimited();
        } else {
            metrics.incAdminApiOutboundFailed();
        }
        const int status_code = capacity_error ? 429 : 500;
        const char* status_text = capacity_error ? "Too Many Requests" : "Internal Server Error";
        spdlog::warn("[Audit][HTTP] outbound-call failed [remote_ip={}, target_uri={}, error={}]",
                     remote_ip, target_uri, call_error.empty() ? "unknown" : call_error);
        std::ostringstream err_body;
        err_body << "{\"error\":\"outbound_call_failed\",\"message\":\""
                 << jsonEscape(call_error.empty() ? "unknown outbound call failure" : call_error)
                 << "\"}";
        return makeHttpResponse(status_code, status_text, err_body.str(), "application/json");
    }

    spdlog::info("[Audit][HTTP] outbound-call accepted [remote_ip={}, target_uri={}, call_id={}]",
                 remote_ip, target_uri, call_id);
    metrics.incAdminApiOutboundAccepted();

    std::ostringstream body;
    body << "{\"status\":\"Accepted\",\"message\":\"Outbound call initiated.\",\"target_uri\":\""
         << jsonEscape(target_uri) << "\",\"call_id\":" << call_id << "}";
    return makeHttpResponse(202, "Accepted", body.str(), "application/json");
}

std::string HttpServer::makeHttpResponse(
    int status_code, const std::string& status_text, const std::string& body,
    const std::string& content_type,
    const std::vector<std::pair<std::string, std::string>>& extra_headers) const
{
    std::ostringstream res;
    res << "HTTP/1.1 " << status_code << " " << status_text << "\r\n"
        << "Content-Type: " << content_type << "\r\n"
        << "Content-Length: " << body.size() << "\r\n";
    for (const auto& header : extra_headers) {
        res << header.first << ": " << header.second << "\r\n";
    }
    res << "Connection: close\r\n\r\n" << body;
    return res.str();
}

bool HttpServer::consumeOutboundRateLimitToken(double* retry_after_seconds) const
{
    const auto& cfg = AppConfig::instance();
    const double rate = static_cast<double>(std::max(1, cfg.admin_api_rate_limit_rps));
    const double burst = static_cast<double>(std::max(1, cfg.admin_api_rate_limit_burst));

    std::lock_guard<std::mutex> lock(rate_limit_mutex_);
    const auto now = std::chrono::steady_clock::now();
    if (!rate_limit_initialized_) {
        rate_limit_tokens_ = burst;
        rate_limit_last_refill_ = now;
        rate_limit_initialized_ = true;
    }

    const std::chrono::duration<double> elapsed = now - rate_limit_last_refill_;
    if (elapsed.count() > 0.0) {
        rate_limit_tokens_ = std::min(burst, rate_limit_tokens_ + elapsed.count() * rate);
        rate_limit_last_refill_ = now;
    }

    if (rate_limit_tokens_ >= 1.0) {
        rate_limit_tokens_ -= 1.0;
        if (retry_after_seconds) {
            *retry_after_seconds = 0.0;
        }
        return true;
    }

    if (retry_after_seconds) {
        *retry_after_seconds = (1.0 - rate_limit_tokens_) / rate;
    }
    return false;
}
