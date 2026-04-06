#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// 전방 선언 (Boost 헤더 꼬임 방지)
namespace boost {
namespace asio {
class io_context;
}  // namespace asio
}  // namespace boost

class VoicebotAccount;

class HttpServer
{
public:
    static HttpServer& getInstance()
    {
        static HttpServer instance;
        return instance;
    }

    // 서버 시작 (백그라운드 스레드에서 수신 대기)
    bool start(int port);

    // 서버 중지
    void stop();

    // Outbound call 실행용 Account 주입 (소유권 없음)
    void setAccount(VoicebotAccount* account);

private:
    HttpServer();
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    void runServer(int port);

    // [A-1 Fix] 개별 연결 처리 — 소켓은 .cpp 내부에서 boost::asio::ip::tcp::socket 사용
    // 헤더에서는 void* 로 타입 숨김 (Boost 헤더 의존성 전파 방지)
    void handleConnectionImpl(void* socket_ptr);

    // 엔드포인트 핸들러
    std::string handleHealthCheck() const;
    std::string handleLiveness() const;
    std::string handleReadiness() const;
    std::string handleMetrics() const;
    std::string handleOutboundCall(const std::string& json_body, const std::string& admin_key,
                                   const std::string& remote_ip) const;
    std::string makeHttpResponse(
        int status_code, const std::string& status_text, const std::string& body,
        const std::string& content_type,
        const std::vector<std::pair<std::string, std::string>>& extra_headers = {}) const;
    bool consumeOutboundRateLimitToken(double* retry_after_seconds) const;

    std::unique_ptr<std::thread> server_thread_;
    std::atomic<bool> is_running_{false};
    std::atomic<int> listen_port_{0};

    mutable std::mutex account_mutex_;
    VoicebotAccount* account_{nullptr};

    mutable std::mutex rate_limit_mutex_;
    mutable bool rate_limit_initialized_{false};
    mutable double rate_limit_tokens_{0.0};
    mutable std::chrono::steady_clock::time_point rate_limit_last_refill_{};

    // [A-1 Fix] 연결당 스레드 풀 — health/metrics 프로브와 outbound API 비간섭
    static constexpr int kMaxWorkerThreads = 8;
    mutable std::mutex workers_mutex_;
    std::vector<std::thread> worker_threads_;
};
