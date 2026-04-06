#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

// Process-wide runtime metrics registry.
// Uses atomics only to keep hot-path updates lock-free.
class RuntimeMetrics
{
public:
    static RuntimeMetrics& instance()
    {
        static RuntimeMetrics m;
        return m;
    }

    void setSipMode(bool pbx_mode) { sip_pbx_mode_.store(pbx_mode, std::memory_order_release); }

    void setSipRegistration(bool is_registered, int status_code)
    {
        sip_registered_.store(is_registered, std::memory_order_release);
        sip_last_status_code_.store(status_code, std::memory_order_release);
    }

    bool sipPbxMode() const { return sip_pbx_mode_.load(std::memory_order_acquire); }

    bool sipRegistered() const { return sip_registered_.load(std::memory_order_acquire); }

    int sipLastStatusCode() const { return sip_last_status_code_.load(std::memory_order_acquire); }

    void incGrpcActiveSessions() { grpc_active_sessions_.fetch_add(1, std::memory_order_relaxed); }

    void decGrpcActiveSessions() { decrementWithFloor(grpc_active_sessions_, 1); }

    std::uint64_t grpcActiveSessions() const
    {
        return grpc_active_sessions_.load(std::memory_order_acquire);
    }

    void addGrpcQueuedFrames(std::size_t delta)
    {
        grpc_queued_frames_.fetch_add(static_cast<std::uint64_t>(delta), std::memory_order_relaxed);
    }

    void subGrpcQueuedFrames(std::size_t delta)
    {
        decrementWithFloor(grpc_queued_frames_, static_cast<std::uint64_t>(delta));
    }

    std::uint64_t grpcQueuedFrames() const
    {
        return grpc_queued_frames_.load(std::memory_order_acquire);
    }

    void incGrpcDroppedFrames()
    {
        grpc_dropped_frames_total_.fetch_add(1, std::memory_order_relaxed);
    }

    std::uint64_t grpcDroppedFramesTotal() const
    {
        return grpc_dropped_frames_total_.load(std::memory_order_acquire);
    }

    void incGrpcReconnectAttempts()
    {
        grpc_reconnect_attempts_total_.fetch_add(1, std::memory_order_relaxed);
    }

    std::uint64_t grpcReconnectAttemptsTotal() const
    {
        return grpc_reconnect_attempts_total_.load(std::memory_order_acquire);
    }

    void incGrpcStreamErrors()
    {
        grpc_stream_errors_total_.fetch_add(1, std::memory_order_relaxed);
    }

    std::uint64_t grpcStreamErrorsTotal() const
    {
        return grpc_stream_errors_total_.load(std::memory_order_acquire);
    }

    void markGrpcHealthy() { grpc_healthy_.store(true, std::memory_order_release); }

    void markGrpcUnhealthy() { grpc_healthy_.store(false, std::memory_order_release); }

    bool grpcHealthy() const { return grpc_healthy_.load(std::memory_order_acquire); }

    void incVadSpeechEvents() { vad_speech_events_total_.fetch_add(1, std::memory_order_relaxed); }

    std::uint64_t vadSpeechEventsTotal() const
    {
        return vad_speech_events_total_.load(std::memory_order_acquire);
    }

    void incBargeInEvents() { barge_in_events_total_.fetch_add(1, std::memory_order_relaxed); }

    std::uint64_t bargeInEventsTotal() const
    {
        return barge_in_events_total_.load(std::memory_order_acquire);
    }

    void incAdminApiOutboundRequests()
    {
        admin_api_outbound_requests_total_.fetch_add(1, std::memory_order_relaxed);
    }

    std::uint64_t adminApiOutboundRequestsTotal() const
    {
        return admin_api_outbound_requests_total_.load(std::memory_order_acquire);
    }

    void incAdminApiOutboundAccepted()
    {
        admin_api_outbound_accepted_total_.fetch_add(1, std::memory_order_relaxed);
    }

    std::uint64_t adminApiOutboundAcceptedTotal() const
    {
        return admin_api_outbound_accepted_total_.load(std::memory_order_acquire);
    }

    void incAdminApiOutboundRejectedAuth()
    {
        admin_api_outbound_rejected_auth_total_.fetch_add(1, std::memory_order_relaxed);
    }

    std::uint64_t adminApiOutboundRejectedAuthTotal() const
    {
        return admin_api_outbound_rejected_auth_total_.load(std::memory_order_acquire);
    }

    void incAdminApiOutboundRejectedRateLimited()
    {
        admin_api_outbound_rejected_rate_limited_total_.fetch_add(1, std::memory_order_relaxed);
    }

    std::uint64_t adminApiOutboundRejectedRateLimitedTotal() const
    {
        return admin_api_outbound_rejected_rate_limited_total_.load(std::memory_order_acquire);
    }

    void incAdminApiOutboundRejectedInvalid()
    {
        admin_api_outbound_rejected_invalid_total_.fetch_add(1, std::memory_order_relaxed);
    }

    std::uint64_t adminApiOutboundRejectedInvalidTotal() const
    {
        return admin_api_outbound_rejected_invalid_total_.load(std::memory_order_acquire);
    }

    void incAdminApiOutboundFailed()
    {
        admin_api_outbound_failed_total_.fetch_add(1, std::memory_order_relaxed);
    }

    std::uint64_t adminApiOutboundFailedTotal() const
    {
        return admin_api_outbound_failed_total_.load(std::memory_order_acquire);
    }

    // Test-only reset utility.
    void resetForTest()
    {
        sip_pbx_mode_.store(false, std::memory_order_release);
        sip_registered_.store(false, std::memory_order_release);
        sip_last_status_code_.store(0, std::memory_order_release);

        grpc_active_sessions_.store(0, std::memory_order_release);
        grpc_queued_frames_.store(0, std::memory_order_release);
        grpc_dropped_frames_total_.store(0, std::memory_order_release);
        grpc_reconnect_attempts_total_.store(0, std::memory_order_release);
        grpc_stream_errors_total_.store(0, std::memory_order_release);
        grpc_healthy_.store(true, std::memory_order_release);

        vad_speech_events_total_.store(0, std::memory_order_release);
        barge_in_events_total_.store(0, std::memory_order_release);
        admin_api_outbound_requests_total_.store(0, std::memory_order_release);
        admin_api_outbound_accepted_total_.store(0, std::memory_order_release);
        admin_api_outbound_rejected_auth_total_.store(0, std::memory_order_release);
        admin_api_outbound_rejected_rate_limited_total_.store(0, std::memory_order_release);
        admin_api_outbound_rejected_invalid_total_.store(0, std::memory_order_release);
        admin_api_outbound_failed_total_.store(0, std::memory_order_release);
    }

private:
    RuntimeMetrics() = default;

    static void decrementWithFloor(std::atomic<std::uint64_t>& target, std::uint64_t delta)
    {
        auto current = target.load(std::memory_order_acquire);
        while (current > 0) {
            std::uint64_t next = (current > delta) ? (current - delta) : 0;
            if (target.compare_exchange_weak(current, next, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
                return;
            }
        }
    }

    std::atomic<bool> sip_pbx_mode_{false};
    std::atomic<bool> sip_registered_{false};
    std::atomic<int> sip_last_status_code_{0};

    std::atomic<std::uint64_t> grpc_active_sessions_{0};
    std::atomic<std::uint64_t> grpc_queued_frames_{0};
    std::atomic<std::uint64_t> grpc_dropped_frames_total_{0};
    std::atomic<std::uint64_t> grpc_reconnect_attempts_total_{0};
    std::atomic<std::uint64_t> grpc_stream_errors_total_{0};
    std::atomic<bool> grpc_healthy_{true};

    std::atomic<std::uint64_t> vad_speech_events_total_{0};
    std::atomic<std::uint64_t> barge_in_events_total_{0};

    std::atomic<std::uint64_t> admin_api_outbound_requests_total_{0};
    std::atomic<std::uint64_t> admin_api_outbound_accepted_total_{0};
    std::atomic<std::uint64_t> admin_api_outbound_rejected_auth_total_{0};
    std::atomic<std::uint64_t> admin_api_outbound_rejected_rate_limited_total_{0};
    std::atomic<std::uint64_t> admin_api_outbound_rejected_invalid_total_{0};
    std::atomic<std::uint64_t> admin_api_outbound_failed_total_{0};
};
