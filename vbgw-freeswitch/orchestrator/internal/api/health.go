/**
 * @file health.go
 * @description 헬스체크 엔드포인트 — /live, /ready, /health, /metrics
 *
 * 변경 이력
 * ─────────────────────────────────────────
 * v1.0.0 | 2026-04-07 | [Implementer] | 최초 생성 | 3-way 헬스체크
 * ─────────────────────────────────────────
 */

package api

import (
	"encoding/json"
	"fmt"
	"net/http"
	"time"

	"vbgw-orchestrator/internal/session"
)

// ESLChecker is the interface HealthHandler needs from ESL client.
type ESLChecker interface {
	IsConnected() bool
}

// HealthHandler holds dependencies for health endpoints.
type HealthHandler struct {
	ESL        ESLChecker
	Sessions   *session.Manager
	BridgeURL  string
	StartTime  time.Time
	httpClient *http.Client
}

// NewHealthHandler creates a HealthHandler.
func NewHealthHandler(eslClient ESLChecker, sessions *session.Manager, bridgeURL string) *HealthHandler {
	return &HealthHandler{
		ESL:       eslClient,
		Sessions:  sessions,
		BridgeURL: bridgeURL,
		StartTime: time.Now(),
		httpClient: &http.Client{Timeout: 2 * time.Second},
	}
}

// Live returns 200 if the process is alive.
func (h *HealthHandler) Live(w http.ResponseWriter, r *http.Request) {
	w.WriteHeader(http.StatusOK)
	fmt.Fprint(w, "OK")
}

// Ready returns 200 if ESL is connected.
func (h *HealthHandler) Ready(w http.ResponseWriter, r *http.Request) {
	if !h.ESL.IsConnected() {
		http.Error(w, `{"status":"not_ready","reason":"ESL disconnected"}`, http.StatusServiceUnavailable)
		return
	}
	w.WriteHeader(http.StatusOK)
	fmt.Fprint(w, "OK")
}

type healthResponse struct {
	Status      string `json:"status"`
	Uptime      string `json:"uptime"`
	ActiveCalls int64  `json:"active_calls"`
	ESL         string `json:"esl"`
	Bridge      string `json:"bridge"`
}

// Health returns a JSON health summary including ESL, Bridge, and session count.
func (h *HealthHandler) Health(w http.ResponseWriter, r *http.Request) {
	resp := healthResponse{
		Status:      "healthy",
		Uptime:      time.Since(h.StartTime).Truncate(time.Second).String(),
		ActiveCalls: h.Sessions.Count(),
	}

	// Check ESL
	if h.ESL.IsConnected() {
		resp.ESL = "connected"
	} else {
		resp.ESL = "disconnected"
		resp.Status = "degraded"
	}

	// Check Bridge
	bridgeResp, err := h.httpClient.Get(h.BridgeURL + "/internal/health")
	if err != nil || bridgeResp.StatusCode != http.StatusOK {
		resp.Bridge = "unreachable"
		resp.Status = "degraded"
	} else {
		resp.Bridge = "healthy"
		bridgeResp.Body.Close()
	}

	w.Header().Set("Content-Type", "application/json")
	if resp.Status != "healthy" {
		w.WriteHeader(http.StatusServiceUnavailable)
	}
	json.NewEncoder(w).Encode(resp)
}
