/**
 * @file retry.go
 * @description gRPC 재연결 — 지수 백오프 (5회, C++ 정책 미러링)
 *
 * 변경 이력
 * ─────────────────────────────────────────
 * v1.0.0 | 2026-04-07 | [Implementer] | 최초 생성 | C++ VoicebotAiClient 재연결 정책 포팅
 * ─────────────────────────────────────────
 */

package grpc

import (
	"context"
	"fmt"
	"log/slog"
	"math/rand"
	"time"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
)

const (
	maxRetries     = 5
	initialBackoff = 100 * time.Millisecond
	maxBackoffMs   = 4000
)

// permanentCodes are gRPC error codes that should not be retried.
var permanentCodes = map[codes.Code]bool{
	codes.InvalidArgument:    true,
	codes.PermissionDenied:   true,
	codes.Unauthenticated:    true,
	codes.FailedPrecondition: true,
	codes.Unimplemented:      true,
	codes.Internal:           true,
	codes.DataLoss:           true,
}

// RetryConnect attempts to connect with exponential backoff.
func RetryConnect(ctx context.Context, pool *Pool) error {
	backoff := initialBackoff

	for attempt := 0; attempt < maxRetries; attempt++ {
		select {
		case <-ctx.Done():
			return ctx.Err()
		default:
		}

		err := pool.Connect(ctx)
		if err == nil {
			return nil
		}

		// Check if error is permanent
		if st, ok := status.FromError(err); ok {
			if permanentCodes[st.Code()] {
				slog.Error("gRPC permanent error, not retrying",
					"code", st.Code(), "msg", st.Message(), "attempt", attempt+1)
				return err
			}
		}

		slog.Warn("gRPC connection failed, retrying",
			"attempt", attempt+1,
			"max_retries", maxRetries,
			"backoff", backoff,
			"err", err,
		)

		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-time.After(backoff):
		}

		// Exponential backoff with jitter
		backoff = time.Duration(min(int64(backoff*2), int64(maxBackoffMs)*int64(time.Millisecond)))
		jitter := time.Duration(rand.Int63n(int64(backoff) / 4))
		backoff += jitter
	}

	return fmt.Errorf("gRPC: max retries (%d) exceeded", maxRetries)
}
