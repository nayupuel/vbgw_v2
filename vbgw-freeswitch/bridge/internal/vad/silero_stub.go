//go:build !cgo

/**
 * @file silero_stub.go
 * @description Silero VAD stub — CGO 없는 환경(로컬 macOS)에서 energy-based VAD 사용
 *
 * 변경 이력
 * ─────────────────────────────────────────
 * v1.0.0 | 2026-04-07 | [Implementer] | 최초 생성 | energy-based stub
 * v1.1.0 | 2026-04-07 | [Implementer] | Phase 2 | 빌드 태그 분리 (!cgo)
 * ─────────────────────────────────────────
 */

package vad

import (
	"log/slog"
	"sync"
)

// Engine wraps the VAD model. Stub mode: energy-based heuristic.
type Engine struct {
	modelPath string
	mu        sync.Mutex
	buffer    []int16
}

// NewEngine creates a VAD engine in stub mode.
func NewEngine(modelPath string) *Engine {
	slog.Warn("VAD engine initialized (STUB mode — CGO disabled, using energy-based detection)",
		"model_path", modelPath)
	return &Engine{
		modelPath: modelPath,
		buffer:    make([]int16, 0, vadWindowSamples*2),
	}
}

// Process takes raw PCM bytes and returns speech detection via energy heuristic.
func (e *Engine) Process(pcmBytes []byte) bool {
	e.mu.Lock()
	defer e.mu.Unlock()

	samples := bytesToInt16(pcmBytes)
	e.buffer = append(e.buffer, samples...)

	var isSpeaking bool
	for len(e.buffer) >= vadWindowSamples {
		window := e.buffer[:vadWindowSamples]
		e.buffer = e.buffer[vadWindowSamples:]
		isSpeaking = energyDetect(window)
	}
	return isSpeaking
}

// Close is a no-op in stub mode.
func (e *Engine) Close() {
	slog.Info("VAD engine closed (stub)")
}

// energyDetect returns true if average absolute amplitude exceeds threshold.
func energyDetect(samples []int16) bool {
	if len(samples) == 0 {
		return false
	}
	var sum int64
	for _, s := range samples {
		if s < 0 {
			sum -= int64(s)
		} else {
			sum += int64(s)
		}
	}
	return sum/int64(len(samples)) > 800
}
