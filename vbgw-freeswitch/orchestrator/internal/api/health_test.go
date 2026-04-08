package api

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"

	"vbgw-orchestrator/internal/session"
)

func TestLive_Returns200(t *testing.T) {
	h := &HealthHandler{}
	req := httptest.NewRequest("GET", "/live", nil)
	w := httptest.NewRecorder()

	h.Live(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d", w.Code)
	}
	if w.Body.String() != "OK" {
		t.Fatalf("expected 'OK', got '%s'", w.Body.String())
	}
}

func TestReady_ESLDisconnected_Returns503(t *testing.T) {
	h := &HealthHandler{
		ESL: eslStub{connected: false},
	}
	req := httptest.NewRequest("GET", "/ready", nil)
	w := httptest.NewRecorder()

	h.Ready(w, req)

	if w.Code != http.StatusServiceUnavailable {
		t.Fatalf("expected 503 when ESL disconnected, got %d", w.Code)
	}
}

func TestReady_ESLConnected_Returns200(t *testing.T) {
	h := &HealthHandler{
		ESL: eslStub{connected: true},
	}
	req := httptest.NewRequest("GET", "/ready", nil)
	w := httptest.NewRecorder()

	h.Ready(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d", w.Code)
	}
}

func TestHealth_ReturnsJSONWithActiveCalls(t *testing.T) {
	sessions := session.NewManager(100)
	s := session.NewSession("s1", "fs1", "010", "100")
	sessions.AddIfUnderCapacity(s)

	// Mock Bridge health endpoint
	bridgeServer := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
	}))
	defer bridgeServer.Close()

	h := NewHealthHandler(eslStub{connected: true}, sessions, bridgeServer.URL)
	req := httptest.NewRequest("GET", "/health", nil)
	w := httptest.NewRecorder()

	h.Health(w, req)

	var resp healthResponse
	if err := json.NewDecoder(w.Body).Decode(&resp); err != nil {
		t.Fatalf("failed to decode JSON: %v", err)
	}

	if resp.ActiveCalls != 1 {
		t.Fatalf("expected 1 active call, got %d", resp.ActiveCalls)
	}
	if resp.ESL != "connected" {
		t.Fatalf("expected ESL=connected, got %s", resp.ESL)
	}
	if resp.Bridge != "healthy" {
		t.Fatalf("expected Bridge=healthy, got %s", resp.Bridge)
	}
	if resp.Status != "healthy" {
		t.Fatalf("expected Status=healthy, got %s", resp.Status)
	}
}

func TestHealth_BridgeDown_ReturnsDegraded(t *testing.T) {
	sessions := session.NewManager(100)
	// No bridge server → connection refused
	h := NewHealthHandler(eslStub{connected: true}, sessions, "http://127.0.0.1:19999")
	req := httptest.NewRequest("GET", "/health", nil)
	w := httptest.NewRecorder()

	h.Health(w, req)

	var resp healthResponse
	json.NewDecoder(w.Body).Decode(&resp)

	if resp.Status != "degraded" {
		t.Fatalf("expected degraded when bridge is down, got %s", resp.Status)
	}
	if resp.Bridge != "unreachable" {
		t.Fatalf("expected Bridge=unreachable, got %s", resp.Bridge)
	}
}

// eslStub implements ESLChecker for testing.
type eslStub struct {
	connected bool
}

func (e eslStub) IsConnected() bool {
	return e.connected
}
