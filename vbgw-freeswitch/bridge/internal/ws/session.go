/**
 * @file session.go
 * @description Per-session 4-goroutine 오케스트레이터 (rx, vad+grpc, ai-response, tx)
 *
 * 변경 이력
 * ─────────────────────────────────────────
 * v1.0.0 | 2026-04-07 | [Implementer] | 최초 생성 | WS↔gRPC 양방향 브릿지
 * v1.1.0 | 2026-04-09 | [Implementer] | T-08,T-18 | 채널 오버플로우 race 수정, ForwardDtmf 에러 반환
 * ─────────────────────────────────────────
 */

package ws

import (
	"context"
	"fmt"
	"log/slog"
	"sync"
	"sync/atomic"

	"github.com/gorilla/websocket"

	"vbgw-bridge/internal/barge"
	grpcclient "vbgw-bridge/internal/grpc"
	"vbgw-bridge/internal/vad"
)

const (
	pcmChCap = 200 // PCM frame channel capacity
	ttsChCap = 200 // TTS frame channel capacity
)

// Session manages the 4-goroutine pipeline for a single WS connection.
type Session struct {
	uuid string
	conn *websocket.Conn

	vadEngine       *vad.Engine
	grpcPool        *grpcclient.Pool
	bargeController *barge.Controller

	pcmCh chan []byte // rx → vad+grpc
	ttsCh chan []byte // ai-response → tx

	aiPaused atomic.Bool

	ctx    context.Context
	cancel context.CancelFunc
}

// NewSession creates a new per-session pipeline.
// parentCtx should be the main application context for proper shutdown propagation.
func NewSession(
	parentCtx context.Context,
	uuid string,
	conn *websocket.Conn,
	vadEngine *vad.Engine,
	grpcPool *grpcclient.Pool,
	bargeCtrl *barge.Controller,
) *Session {
	ctx, cancel := context.WithCancel(parentCtx)
	return &Session{
		uuid:            uuid,
		conn:            conn,
		vadEngine:       vadEngine,
		grpcPool:        grpcPool,
		bargeController: bargeCtrl,
		pcmCh:           make(chan []byte, pcmChCap),
		ttsCh:           make(chan []byte, ttsChCap),
		ctx:             ctx,
		cancel:          cancel,
	}
}

// Run starts all 4 goroutines and waits for completion.
func (s *Session) Run() {
	defer s.cancel()
	defer s.conn.Close()
	defer close(s.pcmCh)
	defer close(s.ttsCh)

	var wg sync.WaitGroup
	wg.Add(4)

	// 1. rx goroutine: WS → pcmCh
	go func() {
		defer wg.Done()
		s.rxLoop()
	}()

	// 2. vad+grpc goroutine: pcmCh → VAD → gRPC send
	go func() {
		defer wg.Done()
		s.vadGrpcLoop()
	}()

	// 3. ai-response goroutine: gRPC recv → ttsCh
	go func() {
		defer wg.Done()
		s.aiResponseLoop()
	}()

	// 4. tx goroutine: ttsCh → WS write
	go func() {
		defer wg.Done()
		s.txLoop()
	}()

	wg.Wait()
}

// SetAIPaused controls whether gRPC send is active.
func (s *Session) SetAIPaused(paused bool) {
	s.aiPaused.Store(paused)
}

// ForwardDtmf sends a DTMF digit to the AI via gRPC AudioChunk.dtmf_digit.
// T-18: Returns error so HTTP handler can report failure to caller.
func (s *Session) ForwardDtmf(digit string) error {
	stream, err := s.grpcPool.GetStream(s.ctx, s.uuid)
	if err != nil {
		slog.Error("ForwardDtmf: gRPC stream unavailable", "uuid", s.uuid, "err", err)
		return fmt.Errorf("stream unavailable: %w", err)
	}
	if err := stream.SendDtmf(s.uuid, digit); err != nil {
		slog.Error("ForwardDtmf: gRPC send failed", "uuid", s.uuid, "err", err)
		return fmt.Errorf("send failed: %w", err)
	}
	slog.Info("DTMF forwarded to gRPC", "uuid", s.uuid, "digit", digit)
	return nil
}

// rxLoop reads PCM frames from WebSocket and sends to pcmCh.
func (s *Session) rxLoop() {
	for {
		select {
		case <-s.ctx.Done():
			return
		default:
		}

		_, data, err := s.conn.ReadMessage()
		if err != nil {
			if websocket.IsCloseError(err, websocket.CloseNormalClosure, websocket.CloseGoingAway) {
				slog.Info("WS closed normally", "uuid", s.uuid)
			} else {
				slog.Error("WS read error", "uuid", s.uuid, "err", err)
			}
			s.cancel()
			return
		}

		// T-08: Non-blocking enqueue — drop new frame if full (atomic, no race)
		select {
		case s.pcmCh <- data:
		default:
			slog.Warn("PCM channel full, dropped incoming frame", "uuid", s.uuid)
		}
	}
}

// vadGrpcLoop processes PCM through VAD and sends to gRPC.
func (s *Session) vadGrpcLoop() {
	stream, err := s.grpcPool.GetStream(s.ctx, s.uuid)
	if err != nil {
		slog.Error("gRPC stream creation failed", "uuid", s.uuid, "err", err)
		s.cancel()
		return
	}

	for {
		select {
		case <-s.ctx.Done():
			return
		case pcm, ok := <-s.pcmCh:
			if !ok {
				return
			}

			if s.aiPaused.Load() {
				continue
			}

			// VAD inference
			isSpeaking := s.vadEngine.Process(pcm)

			// Send to AI via gRPC
			if err := stream.Send(s.uuid, pcm, isSpeaking); err != nil {
				slog.Error("gRPC send failed", "uuid", s.uuid, "err", err)
				s.cancel()
				return
			}
		}
	}
}

// aiResponseLoop receives AI responses from gRPC and routes them.
func (s *Session) aiResponseLoop() {
	stream, err := s.grpcPool.GetStream(s.ctx, s.uuid)
	if err != nil {
		slog.Error("gRPC stream not available for recv", "uuid", s.uuid, "err", err)
		return
	}

	for {
		select {
		case <-s.ctx.Done():
			return
		default:
		}

		resp, err := stream.Recv()
		if err != nil {
			slog.Error("gRPC recv error", "uuid", s.uuid, "err", err)
			s.cancel()
			return
		}

		// Handle clear_buffer (barge-in)
		if resp.ClearBuffer {
			slog.Info("Barge-in: clear_buffer received", "uuid", s.uuid)
			s.bargeController.HandleClearBuffer(s.ctx, s.uuid, s.ttsCh)
		}

		// Route TTS audio to tx channel
		// T-08: Non-blocking enqueue — drop new frame if full (atomic, no race)
		if resp.AudioData != nil && len(resp.AudioData) > 0 {
			select {
			case s.ttsCh <- resp.AudioData:
			default:
				slog.Warn("TTS channel full, dropped incoming frame", "uuid", s.uuid)
			}
		}
	}
}

// txLoop sends TTS audio frames back to FreeSWITCH via WebSocket.
func (s *Session) txLoop() {
	for {
		select {
		case <-s.ctx.Done():
			return
		case frame, ok := <-s.ttsCh:
			if !ok {
				return
			}

			if s.aiPaused.Load() {
				continue
			}

			if err := s.conn.WriteMessage(websocket.BinaryMessage, frame); err != nil {
				slog.Error("WS write error", "uuid", s.uuid, "err", err)
				s.cancel()
				return
			}
		}
	}
}
