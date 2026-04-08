package barge

import (
	"context"
	"net/http"
	"net/http/httptest"
	"sync/atomic"
	"testing"
	"time"

	"vbgw-bridge/internal/tts"
)

func TestHandleClearBuffer_DrainsTTSChannel(t *testing.T) {
	// Setup: TTS channel with 5 pending frames
	ttsCh := make(chan []byte, 200)
	for i := 0; i < 5; i++ {
		tts.Enqueue(ttsCh, []byte("frame"))
	}

	// Mock Orchestrator that accepts barge-in POST
	var received atomic.Int32
	mockOrch := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		received.Add(1)
		w.WriteHeader(http.StatusOK)
	}))
	defer mockOrch.Close()

	ctrl := NewController(mockOrch.URL)
	ctx := context.Background()

	ctrl.HandleClearBuffer(ctx, "test-uuid-123", ttsCh)

	// Verify: TTS channel should be drained
	if len(ttsCh) != 0 {
		t.Fatalf("expected TTS channel drained, got %d frames remaining", len(ttsCh))
	}

	// Verify: Orchestrator received the barge-in request
	if received.Load() != 1 {
		t.Fatalf("expected 1 barge-in request to orchestrator, got %d", received.Load())
	}
}

func TestHandleClearBuffer_EmptyTTSChannel(t *testing.T) {
	ttsCh := make(chan []byte, 200)

	mockOrch := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
	}))
	defer mockOrch.Close()

	ctrl := NewController(mockOrch.URL)
	ctrl.HandleClearBuffer(context.Background(), "test-uuid", ttsCh)

	// Should not panic with empty channel
}

func TestHandleClearBuffer_OrchestratorDown(t *testing.T) {
	ttsCh := make(chan []byte, 200)
	tts.Enqueue(ttsCh, []byte("frame"))

	// Point to a non-existent server
	ctrl := NewController("http://127.0.0.1:19999")
	ctx := context.Background()

	// Should not panic, just log error
	ctrl.HandleClearBuffer(ctx, "test-uuid", ttsCh)

	// TTS should still be drained even if orchestrator is down
	if len(ttsCh) != 0 {
		t.Fatal("TTS should be drained even when orchestrator is unreachable")
	}
}

func TestHandleClearBuffer_ContextCancelled(t *testing.T) {
	ttsCh := make(chan []byte, 200)
	tts.Enqueue(ttsCh, []byte("frame"))

	mockOrch := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
	}))
	defer mockOrch.Close()

	ctx, cancel := context.WithCancel(context.Background())
	cancel() // Already cancelled

	ctrl := NewController(mockOrch.URL)
	ctrl.HandleClearBuffer(ctx, "test-uuid", ttsCh)

	// Should drain TTS but HTTP request may fail (cancelled context)
	if len(ttsCh) != 0 {
		t.Fatal("TTS should be drained even with cancelled context")
	}
}

func TestHandleClearBuffer_MeasuresLatency(t *testing.T) {
	ttsCh := make(chan []byte, 200)
	for i := 0; i < 10; i++ {
		tts.Enqueue(ttsCh, []byte("frame"))
	}

	mockOrch := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		time.Sleep(10 * time.Millisecond) // Simulate network latency
		w.WriteHeader(http.StatusOK)
	}))
	defer mockOrch.Close()

	ctrl := NewController(mockOrch.URL)
	start := time.Now()
	ctrl.HandleClearBuffer(context.Background(), "test-uuid", ttsCh)
	elapsed := time.Since(start)

	// Barge-in total time should be measurable (drain + HTTP POST)
	if elapsed < 10*time.Millisecond {
		t.Fatalf("expected at least 10ms latency, got %v", elapsed)
	}
	// But should complete within reasonable time
	if elapsed > 5*time.Second {
		t.Fatalf("barge-in took too long: %v", elapsed)
	}
}
