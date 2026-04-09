/**
 * @file main.go
 * @description Bridge 진입점 — WS 서버 + Internal HTTP + gRPC 연결
 *
 * 변경 이력
 * ─────────────────────────────────────────
 * v1.0.0 | 2026-04-07 | [Implementer] | 최초 생성 | Phase 1 PoC 엔트리포인트
 * v1.0.1 | 2026-04-09 | [Implementer] | T-21 | gRPC 초기 연결 실패 시 Error 로그
 * ─────────────────────────────────────────
 */

package main

import (
	"context"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"vbgw-bridge/internal/barge"
	"vbgw-bridge/internal/config"
	grpcclient "vbgw-bridge/internal/grpc"
	"vbgw-bridge/internal/vad"
	"vbgw-bridge/internal/ws"
)

func main() {
	cfg := config.Load()
	setupLogging(cfg.LogLevel)

	slog.Info("Bridge starting",
		"ws_port", cfg.WSPort,
		"internal_port", cfg.InternalPort,
		"ai_grpc_addr", cfg.AIGrpcAddr,
	)

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	// Initialize VAD engine (stub for Phase 1)
	vadEngine := vad.NewEngine(cfg.OnnxModelPath)
	defer vadEngine.Close()

	// Initialize gRPC pool
	grpcPool := grpcclient.NewPool(cfg.AIGrpcAddr, cfg.AIGrpcTLS)
	// T-21: Log at Error level (not Warn) — all calls will fail until AI engine is reachable
	if err := grpcclient.RetryConnect(ctx, grpcPool); err != nil {
		slog.Error("gRPC initial connection failed — calls will fail until AI engine is reachable",
			"err", err, "ai_grpc_addr", cfg.AIGrpcAddr)
	}
	defer grpcPool.Close()

	// Initialize barge-in controller
	bargeCtrl := barge.NewController(cfg.OrchestratorURL)

	// Initialize WS server
	wsServer := ws.NewServer(ctx, vadEngine, grpcPool, bargeCtrl)

	// WS HTTP server (port 8090 — mod_audio_fork connects here)
	wsMux := http.NewServeMux()
	wsMux.HandleFunc("/audio/", wsServer.HandleAudio)

	wsHTTP := &http.Server{
		Addr:    fmt.Sprintf(":%d", cfg.WSPort),
		Handler: wsMux,
	}

	// Internal HTTP server (port 8091 — Orchestrator connects here)
	internalHTTP := &http.Server{
		Addr:    fmt.Sprintf(":%d", cfg.InternalPort),
		Handler: wsServer.InternalHandler(),
	}

	// Start servers
	go func() {
		slog.Info("WS server listening", "port", cfg.WSPort)
		if err := wsHTTP.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			slog.Error("WS server error", "err", err)
			os.Exit(1)
		}
	}()

	go func() {
		slog.Info("Internal HTTP server listening", "port", cfg.InternalPort)
		if err := internalHTTP.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			slog.Error("Internal HTTP server error", "err", err)
			os.Exit(1)
		}
	}()

	// Graceful shutdown
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)

	<-sigCh
	slog.Info("Bridge shutting down...")

	shutdownCtx, shutdownCancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer shutdownCancel()

	wsHTTP.Shutdown(shutdownCtx)
	internalHTTP.Shutdown(shutdownCtx)
	cancel()

	slog.Info("Bridge shutdown complete")
}

func setupLogging(level string) {
	var logLevel slog.Level
	switch level {
	case "debug":
		logLevel = slog.LevelDebug
	case "warn":
		logLevel = slog.LevelWarn
	case "error":
		logLevel = slog.LevelError
	default:
		logLevel = slog.LevelInfo
	}
	slog.SetDefault(slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{Level: logLevel})))
}
