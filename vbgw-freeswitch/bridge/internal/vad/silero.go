//go:build cgo

/**
 * @file silero.go
 * @description Silero VAD v4 ONNX 추론 엔진 — onnxruntime-go 바인딩
 *
 * 변경 이력
 * ─────────────────────────────────────────
 * v1.0.0 | 2026-04-07 | [Implementer] | 최초 생성 | energy-based stub
 * v1.1.0 | 2026-04-07 | [Implementer] | Phase 2 | 실제 ONNX 추론 구현 (cgo 빌드 태그)
 * v1.2.0 | 2026-04-09 | [Implementer] | T-01 | infer() 입력 텐서 업데이트 수정 + ORT_LIB_PATH 환경변수
 * ─────────────────────────────────────────
 */

package vad

import (
	"fmt"
	"log/slog"
	"os"
	"sync"

	ort "github.com/yalue/onnxruntime_go"
)

const (
	// vadThreshold is the probability threshold for speech detection.
	vadThreshold = 0.5

	// sampleRate for Silero VAD v4.
	sampleRate = 16000
)

// Engine wraps the Silero VAD ONNX model with onnxruntime-go.
type Engine struct {
	modelPath string
	session   *ort.AdvancedSession
	mu        sync.Mutex
	buffer    []int16

	// T-01: Input tensor (kept as field so infer() can update its data)
	input *ort.Tensor[float32] // [1, vadWindowSamples]

	// Silero VAD v4 state tensors (LSTM hidden states)
	h  *ort.Tensor[float32] // [2, 1, 64]
	c  *ort.Tensor[float32] // [2, 1, 64]
	sr *ort.Tensor[int64]   // [1] = 16000

	// Output tensor
	output *ort.Tensor[float32] // [1, 1]
	hn     *ort.Tensor[float32] // [2, 1, 64]
	cn     *ort.Tensor[float32] // [2, 1, 64]
}

// NewEngine creates a VAD engine with ONNX Runtime.
func NewEngine(modelPath string) *Engine {
	e := &Engine{
		modelPath: modelPath,
		buffer:    make([]int16, 0, vadWindowSamples*2),
	}

	if err := e.loadModel(); err != nil {
		slog.Error("ONNX VAD model load failed, falling back to energy-based",
			"model_path", modelPath, "err", err)
		return e
	}

	slog.Info("VAD engine initialized (ONNX mode)", "model_path", modelPath)
	return e
}

func (e *Engine) loadModel() error {
	ort.SetSharedLibraryPath(getOrtLibPath())
	if err := ort.InitializeEnvironment(); err != nil {
		return fmt.Errorf("ONNX env init: %w", err)
	}

	// Silero VAD v4 input shapes
	inputShape := ort.NewShape(1, vadWindowSamples)
	srShape := ort.NewShape(1)
	hShape := ort.NewShape(2, 1, 64)
	cShape := ort.NewShape(2, 1, 64)
	outputShape := ort.NewShape(1, 1)

	// Create input tensors
	e.input, err = ort.NewEmptyTensor[float32](inputShape)
	if err != nil {
		return fmt.Errorf("input tensor: %w", err)
	}

	e.sr, err = ort.NewTensor(srShape, []int64{sampleRate})
	if err != nil {
		return fmt.Errorf("sr tensor: %w", err)
	}

	e.h, err = ort.NewEmptyTensor[float32](hShape)
	if err != nil {
		return fmt.Errorf("h tensor: %w", err)
	}

	e.c, err = ort.NewEmptyTensor[float32](cShape)
	if err != nil {
		return fmt.Errorf("c tensor: %w", err)
	}

	// Create output tensors
	e.output, err = ort.NewEmptyTensor[float32](outputShape)
	if err != nil {
		return fmt.Errorf("output tensor: %w", err)
	}

	e.hn, err = ort.NewEmptyTensor[float32](hShape)
	if err != nil {
		return fmt.Errorf("hn tensor: %w", err)
	}

	e.cn, err = ort.NewEmptyTensor[float32](cShape)
	if err != nil {
		return fmt.Errorf("cn tensor: %w", err)
	}

	// Create session
	// Silero VAD v4 inputs: input, sr, h, c
	// Silero VAD v4 outputs: output, hn, cn
	inputs := []ort.ArbitraryTensor{e.input, e.sr, e.h, e.c}
	outputs := []ort.ArbitraryTensor{e.output, e.hn, e.cn}

	e.session, err = ort.NewAdvancedSession(
		e.modelPath,
		[]string{"input", "sr", "h", "c"},
		[]string{"output", "hn", "cn"},
		inputs,
		outputs,
		nil,
	)
	if err != nil {
		return fmt.Errorf("ONNX session: %w", err)
	}

	return nil
}

// Process takes raw PCM bytes (L16, 16kHz, mono) and returns speech detection result.
func (e *Engine) Process(pcmBytes []byte) bool {
	e.mu.Lock()
	defer e.mu.Unlock()

	samples := bytesToInt16(pcmBytes)
	e.buffer = append(e.buffer, samples...)

	var isSpeaking bool
	for len(e.buffer) >= vadWindowSamples {
		window := e.buffer[:vadWindowSamples]
		e.buffer = e.buffer[vadWindowSamples:]

		if e.session != nil {
			isSpeaking = e.infer(window)
		} else {
			isSpeaking = energyDetectFallback(window)
		}
	}
	return isSpeaking
}

// infer runs ONNX inference on a 512-sample window.
// T-01: Writes normalized samples directly into the pre-allocated input tensor,
// then runs inference. Reuses e.session — does NOT create a new session per call.
func (e *Engine) infer(samples []int16) bool {
	if e.session == nil || e.input == nil {
		return energyDetectFallback(samples)
	}

	// T-01 FIX: Write normalized samples directly into the input tensor's backing data.
	// Previously created a local []float32 that was never attached to the session.
	inputSlice := e.input.GetData()
	for i, s := range samples {
		inputSlice[i] = float32(s) / 32768.0
	}

	// Copy h/c state from previous hn/cn output (LSTM state propagation)
	copy(e.h.GetData(), e.hn.GetData())
	copy(e.c.GetData(), e.cn.GetData())

	// Run inference using the pre-loaded session
	if err := e.session.Run(); err != nil {
		slog.Error("VAD inference run failed", "err", err)
		return energyDetectFallback(samples)
	}

	prob := e.output.GetData()[0]
	return prob > vadThreshold
}

// Close releases ONNX resources.
func (e *Engine) Close() {
	if e.session != nil {
		e.session.Destroy()
	}
	if e.input != nil {
		e.input.Destroy()
	}
	if e.h != nil {
		e.h.Destroy()
	}
	if e.c != nil {
		e.c.Destroy()
	}
	if e.sr != nil {
		e.sr.Destroy()
	}
	if e.output != nil {
		e.output.Destroy()
	}
	if e.hn != nil {
		e.hn.Destroy()
	}
	if e.cn != nil {
		e.cn.Destroy()
	}
	ort.DestroyEnvironment()
	slog.Info("VAD engine closed (ONNX)")
}

// energyDetectFallback is used when ONNX load fails.
func energyDetectFallback(samples []int16) bool {
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

func getOrtLibPath() string {
	if p := os.Getenv("ORT_LIB_PATH"); p != "" {
		return p
	}
	return "/usr/local/lib/libonnxruntime.so"
}
