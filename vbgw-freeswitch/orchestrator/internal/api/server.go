/**
 * @file server.go
 * @description HTTP API 서버 — chi 라우터, 12 엔드포인트 등록
 *
 * 변경 이력
 * ─────────────────────────────────────────
 * v1.0.0 | 2026-04-07 | [Implementer] | 최초 생성 | 12 엔드포인트 + Prometheus
 * v1.1.0 | 2026-04-07 | [Implementer] | Phase 3 | pprof 엔드포인트 추가
 * v1.2.0 | 2026-04-09 | [Implementer] | T-27 | /health를 auth 그룹으로 이동
 * ─────────────────────────────────────────
 */

package api

import (
	"fmt"
	"net/http"
	"net/http/pprof"
	"time"

	"vbgw-orchestrator/internal/config"
	"vbgw-orchestrator/internal/esl"
	"vbgw-orchestrator/internal/session"

	"github.com/go-chi/chi/v5"
	"github.com/prometheus/client_golang/prometheus/promhttp"
)

// NewRouter creates the HTTP router with all endpoints registered.
func NewRouter(cfg *config.Config, eslClient *esl.Client, sessions *session.Manager) http.Handler {
	r := chi.NewRouter()

	bridgeURL := "http://" + cfg.BridgeHost + ":" + itoa(cfg.BridgeInternalPort)
	httpClient := &http.Client{Timeout: 5 * time.Second}

	healthHandler := NewHealthHandler(eslClient, sessions, bridgeURL)
	callsHandler := &CallsHandler{
		ESL:          eslClient,
		Sessions:     sessions,
		UseStandbyGW: cfg.PBXStandbyHost != "",
	}
	controlHandler := &ControlHandler{
		ESL:        eslClient,
		Sessions:   sessions,
		BridgeURL:  bridgeURL,
		httpClient: httpClient,
	}
	statsHandler := &StatsHandler{ESL: eslClient, Sessions: sessions}

	// Public health endpoints (no auth — liveness/readiness probes)
	r.Get("/live", healthHandler.Live)
	r.Get("/ready", healthHandler.Ready)
	// T-27: /health moved to auth group (exposes active_calls, component status)

	// Protected: metrics + API endpoints (require auth)
	r.Group(func(r chi.Router) {
		r.Use(MetricsMiddleware)
		r.Use(RateLimitMiddleware(cfg.RateLimitRPS, cfg.RateLimitBurst))
		r.Use(AuthMiddleware(cfg.AdminAPIKey))

		// T-27: /health behind auth (exposes internal component status)
		r.Get("/health", healthHandler.Health)

		// Prometheus metrics (behind auth to prevent info leak)
		r.Handle("/metrics", promhttp.Handler())

		// E-05: POST /api/v1/calls
		r.Post("/api/v1/calls", callsHandler.CreateCall)

		// E-06: POST /api/v1/calls/{id}/dtmf
		r.Post("/api/v1/calls/{id}/dtmf", controlHandler.SendDtmf)

		// E-07: POST /api/v1/calls/{id}/transfer
		r.Post("/api/v1/calls/{id}/transfer", controlHandler.Transfer)

		// E-08: POST /api/v1/calls/{id}/record/start
		r.Post("/api/v1/calls/{id}/record/start", controlHandler.RecordStart)

		// E-09: POST /api/v1/calls/{id}/record/stop
		r.Post("/api/v1/calls/{id}/record/stop", controlHandler.RecordStop)

		// E-10: GET /api/v1/calls/{id}/stats
		r.Get("/api/v1/calls/{id}/stats", statsHandler.GetStats)

		// E-11: POST /api/v1/calls/bridge
		r.Post("/api/v1/calls/bridge", controlHandler.BridgeCalls)

		// E-12: POST /api/v1/calls/unbridge
		r.Post("/api/v1/calls/unbridge", controlHandler.UnbridgeCalls)
	})

	// Internal endpoints (loopback only — Bridge → Orchestrator)
	r.Group(func(r chi.Router) {
		r.Use(LoopbackOnlyMiddleware)
		r.Post("/internal/barge-in/{uuid}", controlHandler.BargeIn)
	})

	// Debug: pprof endpoints (non-production only, behind auth)
	if cfg.RuntimeProfile != "production" {
		r.Group(func(r chi.Router) {
			r.Use(AuthMiddleware(cfg.AdminAPIKey))
			r.HandleFunc("/debug/pprof/", pprof.Index)
			r.HandleFunc("/debug/pprof/cmdline", pprof.Cmdline)
			r.HandleFunc("/debug/pprof/profile", pprof.Profile)
			r.HandleFunc("/debug/pprof/symbol", pprof.Symbol)
			r.HandleFunc("/debug/pprof/trace", pprof.Trace)
			r.Handle("/debug/pprof/heap", pprof.Handler("heap"))
			r.Handle("/debug/pprof/goroutine", pprof.Handler("goroutine"))
			r.Handle("/debug/pprof/allocs", pprof.Handler("allocs"))
		})
	}

	return r
}

func itoa(i int) string {
	return fmt.Sprintf("%d", i)
}
