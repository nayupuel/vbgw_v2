/**
 * @file client.go
 * @description gRPC bidirectional streaming 클라이언트 — voicebot.proto 실제 연동
 *
 * 변경 이력
 * ─────────────────────────────────────────
 * v1.0.0 | 2026-04-07 | [Implementer] | 최초 생성 | StreamSession 양방향 스트리밍
 * v1.1.0 | 2026-04-07 | [Implementer] | Phase 2 | stub → 실제 proto StreamSession 연동
 * v1.2.0 | 2026-04-09 | [Implementer] | T-04,T-09 | CloseSend 추가, GetStream 중복 방지
 * ─────────────────────────────────────────
 */

package grpc

import (
	"context"
	"fmt"
	"io"
	"log/slog"
	"sync"

	pb "vbgw-bridge/proto/voicebot"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
)

// AiResponse represents a parsed response from the AI engine.
type AiResponse struct {
	Type        int32
	TextContent string
	AudioData   []byte
	ClearBuffer bool
}

// Stream wraps a gRPC bidirectional stream for a single session.
type Stream struct {
	uuid       string
	grpcStream grpc.BidiStreamingClient[pb.AudioChunk, pb.AiResponse]
	sendCh     chan sendMsg
	ctx        context.Context
	cancel     context.CancelFunc
	recvCh     chan *AiResponse
}

type sendMsg struct {
	sessionID  string
	audioData  []byte
	isSpeaking bool
	dtmfDigit  string
}

// Send queues an audio chunk to be sent to the AI engine.
func (s *Stream) Send(sessionID string, audioData []byte, isSpeaking bool) error {
	select {
	case s.sendCh <- sendMsg{sessionID: sessionID, audioData: audioData, isSpeaking: isSpeaking}:
		return nil
	case <-s.ctx.Done():
		return s.ctx.Err()
	}
}

// SendDtmf queues a DTMF digit to be sent to the AI engine.
func (s *Stream) SendDtmf(sessionID, digit string) error {
	select {
	case s.sendCh <- sendMsg{sessionID: sessionID, dtmfDigit: digit}:
		return nil
	case <-s.ctx.Done():
		return s.ctx.Err()
	}
}

// Recv blocks until an AI response is available.
func (s *Stream) Recv() (*AiResponse, error) {
	select {
	case resp, ok := <-s.recvCh:
		if !ok {
			return nil, fmt.Errorf("recv channel closed")
		}
		return resp, nil
	case <-s.ctx.Done():
		return nil, s.ctx.Err()
	}
}

// Pool manages gRPC connections and per-session streams.
type Pool struct {
	addr    string
	useTLS  bool
	conn    *grpc.ClientConn
	client  pb.VoicebotAiServiceClient
	streams sync.Map // map[string]*Stream
	mu      sync.Mutex
}

// NewPool creates a gRPC connection pool.
func NewPool(addr string, useTLS bool) *Pool {
	return &Pool{addr: addr, useTLS: useTLS}
}

// Connect establishes the gRPC connection and creates the service client.
func (p *Pool) Connect(ctx context.Context) error {
	p.mu.Lock()
	defer p.mu.Unlock()

	opts := []grpc.DialOption{
		grpc.WithDefaultCallOptions(grpc.MaxCallRecvMsgSize(16 * 1024 * 1024)),
	}

	if !p.useTLS {
		opts = append(opts, grpc.WithTransportCredentials(insecure.NewCredentials()))
	}

	conn, err := grpc.NewClient(p.addr, opts...)
	if err != nil {
		return fmt.Errorf("gRPC dial failed: %w", err)
	}

	p.conn = conn
	p.client = pb.NewVoicebotAiServiceClient(conn)
	slog.Info("gRPC connected", "addr", p.addr)
	return nil
}

// GetStream returns or creates a bidirectional stream for the given UUID.
// T-09: Always uses lock to prevent duplicate stream creation race.
func (p *Pool) GetStream(ctx context.Context, uuid string) (*Stream, error) {
	// Fast path: check cache without lock (read-only, safe with sync.Map)
	if v, ok := p.streams.Load(uuid); ok {
		s := v.(*Stream)
		select {
		case <-s.ctx.Done():
			// Stale — fall through to locked path
		default:
			return s, nil
		}
	}

	// Lock to prevent duplicate creation
	p.mu.Lock()
	defer p.mu.Unlock()

	// Double-check after acquiring lock (another goroutine may have created it)
	if v, ok := p.streams.Load(uuid); ok {
		s := v.(*Stream)
		select {
		case <-s.ctx.Done():
			p.streams.Delete(uuid) // Remove stale under lock, then recreate below
		default:
			return s, nil
		}
	}

	if p.client == nil {
		return nil, fmt.Errorf("gRPC client not connected")
	}

	streamCtx, streamCancel := context.WithCancel(ctx)

	// Create actual gRPC bidirectional stream
	grpcStream, err := p.client.StreamSession(streamCtx)
	if err != nil {
		streamCancel()
		return nil, fmt.Errorf("gRPC StreamSession failed: %w", err)
	}

	s := &Stream{
		uuid:       uuid,
		grpcStream: grpcStream,
		sendCh:     make(chan sendMsg, 200),
		recvCh:     make(chan *AiResponse, 200),
		ctx:        streamCtx,
		cancel:     streamCancel,
	}

	p.streams.Store(uuid, s)
	slog.Info("gRPC stream created", "uuid", uuid)

	go p.streamSendLoop(s)
	go p.streamRecvLoop(s)

	return s, nil
}

// RemoveStream closes and removes a stream.
func (p *Pool) RemoveStream(uuid string) {
	if v, loaded := p.streams.LoadAndDelete(uuid); loaded {
		s := v.(*Stream)
		s.grpcStream.CloseSend()
		s.cancel()
		slog.Info("gRPC stream removed", "uuid", uuid)
	}
}

// Close shuts down the gRPC connection.
func (p *Pool) Close() {
	p.streams.Range(func(key, value any) bool {
		s := value.(*Stream)
		s.grpcStream.CloseSend()
		s.cancel()
		return true
	})
	if p.conn != nil {
		p.conn.Close()
	}
}

// streamSendLoop reads from sendCh and sends AudioChunk to gRPC.
func (p *Pool) streamSendLoop(s *Stream) {
	// T-04: Always CloseSend on exit to notify server that send is done
	defer func() {
		if err := s.grpcStream.CloseSend(); err != nil {
			slog.Error("gRPC CloseSend failed", "uuid", s.uuid, "err", err)
		}
		slog.Info("gRPC send loop exited", "uuid", s.uuid)
	}()

	for {
		select {
		case <-s.ctx.Done():
			return
		case msg, ok := <-s.sendCh:
			if !ok {
				return
			}

			chunk := &pb.AudioChunk{
				SessionId:  msg.sessionID,
				AudioData:  msg.audioData,
				IsSpeaking: msg.isSpeaking,
				DtmfDigit:  msg.dtmfDigit,
			}

			if err := s.grpcStream.Send(chunk); err != nil {
				slog.Error("gRPC send failed", "uuid", s.uuid, "err", err)
				s.cancel()
				return
			}
		}
	}
}

// streamRecvLoop reads AiResponse from gRPC and sends to recvCh.
func (p *Pool) streamRecvLoop(s *Stream) {
	defer func() {
		close(s.recvCh)
		slog.Info("gRPC recv loop exited", "uuid", s.uuid)
	}()

	for {
		resp, err := s.grpcStream.Recv()
		if err != nil {
			if err == io.EOF || s.ctx.Err() != nil {
				return
			}
			slog.Error("gRPC recv failed", "uuid", s.uuid, "err", err)
			s.cancel()
			return
		}

		aiResp := &AiResponse{
			Type:        int32(resp.Type),
			TextContent: resp.TextContent,
			AudioData:   resp.AudioData,
			ClearBuffer: resp.ClearBuffer,
		}

		select {
		case s.recvCh <- aiResp:
		case <-s.ctx.Done():
			return
		}
	}
}
