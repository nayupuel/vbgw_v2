#include "utils/RuntimeMetrics.h"

#include <gtest/gtest.h>

TEST(RuntimeMetricsTest, TracksSipAndGrpcState)
{
    auto& metrics = RuntimeMetrics::instance();
    metrics.resetForTest();

    metrics.setSipMode(true);
    metrics.setSipRegistration(true, 200);
    EXPECT_TRUE(metrics.sipPbxMode());
    EXPECT_TRUE(metrics.sipRegistered());
    EXPECT_EQ(metrics.sipLastStatusCode(), 200);

    metrics.incGrpcActiveSessions();
    metrics.addGrpcQueuedFrames(3);
    metrics.incGrpcDroppedFrames();
    metrics.incGrpcReconnectAttempts();
    metrics.incGrpcStreamErrors();

    EXPECT_EQ(metrics.grpcActiveSessions(), 1u);
    EXPECT_EQ(metrics.grpcQueuedFrames(), 3u);
    EXPECT_EQ(metrics.grpcDroppedFramesTotal(), 1u);
    EXPECT_EQ(metrics.grpcReconnectAttemptsTotal(), 1u);
    EXPECT_EQ(metrics.grpcStreamErrorsTotal(), 1u);
}

TEST(RuntimeMetricsTest, QueueDepthFloorAtZero)
{
    auto& metrics = RuntimeMetrics::instance();
    metrics.resetForTest();

    metrics.addGrpcQueuedFrames(2);
    metrics.subGrpcQueuedFrames(10);

    EXPECT_EQ(metrics.grpcQueuedFrames(), 0u);

    metrics.incGrpcActiveSessions();
    metrics.decGrpcActiveSessions();
    metrics.decGrpcActiveSessions();
    EXPECT_EQ(metrics.grpcActiveSessions(), 0u);
}

TEST(RuntimeMetricsTest, TracksVadAndBargeInCounters)
{
    auto& metrics = RuntimeMetrics::instance();
    metrics.resetForTest();

    metrics.incVadSpeechEvents();
    metrics.incBargeInEvents();
    metrics.incBargeInEvents();

    EXPECT_EQ(metrics.vadSpeechEventsTotal(), 1u);
    EXPECT_EQ(metrics.bargeInEventsTotal(), 2u);
}

TEST(RuntimeMetricsTest, TracksAdminApiCounters)
{
    auto& metrics = RuntimeMetrics::instance();
    metrics.resetForTest();

    metrics.incAdminApiOutboundRequests();
    metrics.incAdminApiOutboundRequests();
    metrics.incAdminApiOutboundAccepted();
    metrics.incAdminApiOutboundRejectedAuth();
    metrics.incAdminApiOutboundRejectedRateLimited();
    metrics.incAdminApiOutboundRejectedInvalid();
    metrics.incAdminApiOutboundFailed();

    EXPECT_EQ(metrics.adminApiOutboundRequestsTotal(), 2u);
    EXPECT_EQ(metrics.adminApiOutboundAcceptedTotal(), 1u);
    EXPECT_EQ(metrics.adminApiOutboundRejectedAuthTotal(), 1u);
    EXPECT_EQ(metrics.adminApiOutboundRejectedRateLimitedTotal(), 1u);
    EXPECT_EQ(metrics.adminApiOutboundRejectedInvalidTotal(), 1u);
    EXPECT_EQ(metrics.adminApiOutboundFailedTotal(), 1u);
}
