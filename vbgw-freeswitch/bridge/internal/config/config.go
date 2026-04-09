/**
 * @file config.go
 * @description Bridge 환경변수 설정 로더
 *
 * 변경 이력
 * ─────────────────────────────────────────
 * v1.0.0 | 2026-04-07 | [Implementer] | 최초 생성 | WS, gRPC, VAD 설정
 * ─────────────────────────────────────────
 */

package config

import (
	"os"
	"strconv"
)

type Config struct {
	// WebSocket (from FS mod_audio_fork)
	WSPort int

	// Internal HTTP (from Orchestrator)
	InternalPort int

	// AI Engine (gRPC)
	AIGrpcAddr string
	AIGrpcTLS  bool

	// VAD
	OnnxModelPath string

	// gRPC Retry
	GrpcMaxRetries      int
	GrpcMaxBackoffMs    int
	GrpcStreamDeadlineS int

	// Orchestrator (for barge-in callback)
	OrchestratorURL string

	// Logging
	LogLevel string
}

func Load() *Config {
	return &Config{
		WSPort:           envInt("WS_PORT", 8090),
		InternalPort:     envInt("INTERNAL_PORT", 8091),
		AIGrpcAddr:       envStr("AI_GRPC_ADDR", "127.0.0.1:50051"),
		AIGrpcTLS:        envBool("AI_GRPC_TLS", false),
		OnnxModelPath:    envStr("ONNX_MODEL_PATH", "/models/silero_vad.onnx"),
		GrpcMaxRetries:   envInt("GRPC_MAX_RETRIES", 5),
		GrpcMaxBackoffMs: envInt("GRPC_MAX_BACKOFF_MS", 4000),
		// T-28: Reduced from 86400 (24h) to 7200 (2h) to prevent zombie streams
		GrpcStreamDeadlineS: envInt("GRPC_STREAM_DEADLINE_SECS", 7200),
		OrchestratorURL:     envStr("ORCHESTRATOR_URL", "http://127.0.0.1:8080"),
		LogLevel:            envStr("LOG_LEVEL", "info"),
	}
}

func envStr(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}

func envInt(key string, fallback int) int {
	if v := os.Getenv(key); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			return n
		}
	}
	return fallback
}

func envBool(key string, fallback bool) bool {
	if v := os.Getenv(key); v != "" {
		if b, err := strconv.ParseBool(v); err == nil {
			return b
		}
	}
	return fallback
}
