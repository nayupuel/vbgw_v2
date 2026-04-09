/**
 * @file server.go
 * @description WebSocket 서버 — HTTP 업그레이드 + per-UUID 라우팅
 *
 * 변경 이력
 * ─────────────────────────────────────────
 * v1.0.0 | 2026-04-07 | [Implementer] | 최초 생성 | /audio/{uuid} WS 엔드포인트
 * v1.0.1 | 2026-04-09 | [Implementer] | T-18 | DTMF 에러 반환, shutdown 엔드포인트
 * ─────────────────────────────────────────
 */

package ws

import (
	"context"
	"encoding/json"
	"log/slog"
	"net/http"
	"strings"
	"sync"

	"github.com/gorilla/websocket"

	"vbgw-bridge/internal/barge"
	grpcclient "vbgw-bridge/internal/grpc"
	"vbgw-bridge/internal/vad"
)

var upgrader = websocket.Upgrader{
	ReadBufferSize:  8192,
	WriteBufferSize: 8192,
	CheckOrigin: func(r *http.Request) bool {
		// Only allow loopback connections (FreeSWITCH mod_audio_fork)
		remoteIP := r.RemoteAddr
		if idx := strings.LastIndex(remoteIP, ":"); idx >= 0 {
			remoteIP = remoteIP[:idx]
		}
		remoteIP = strings.Trim(remoteIP, "[]")
		return remoteIP == "127.0.0.1" || remoteIP == "::1"
	},
}

// Server manages WebSocket connections from FreeSWITCH mod_audio_fork.
type Server struct {
	ctx      context.Context
	sessions sync.Map // map[string]*Session (key: uuid)

	vadEngine       *vad.Engine
	grpcClientPool  *grpcclient.Pool
	bargeController *barge.Controller
}

// NewServer creates a WS server with the given parent context.
func NewServer(ctx context.Context, vadEngine *vad.Engine, grpcPool *grpcclient.Pool, bargeCtrl *barge.Controller) *Server {
	return &Server{
		ctx:             ctx,
		vadEngine:       vadEngine,
		grpcClientPool:  grpcPool,
		bargeController: bargeCtrl,
	}
}

// HandleAudio handles the WS upgrade for /audio/{uuid}.
func (s *Server) HandleAudio(w http.ResponseWriter, r *http.Request) {
	// Extract UUID from path: /audio/{uuid}
	parts := strings.Split(r.URL.Path, "/")
	if len(parts) < 3 {
		http.Error(w, "missing uuid", http.StatusBadRequest)
		return
	}
	uuid := parts[len(parts)-1]

	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		slog.Error("WS upgrade failed", "uuid", uuid, "err", err)
		return
	}

	slog.Info("WS connection established", "uuid", uuid)

	sess := NewSession(s.ctx, uuid, conn, s.vadEngine, s.grpcClientPool, s.bargeController)
	s.sessions.Store(uuid, sess)

	go func() {
		sess.Run()
		s.sessions.Delete(uuid)
		slog.Info("WS session ended", "uuid", uuid)
	}()
}

// GetSession returns an active session by UUID.
func (s *Server) GetSession(uuid string) (*Session, bool) {
	v, ok := s.sessions.Load(uuid)
	if !ok {
		return nil, false
	}
	return v.(*Session), true
}

// PauseAI pauses the AI gRPC send for a session (bridge mode).
func (s *Server) PauseAI(uuid string) {
	if sess, ok := s.GetSession(uuid); ok {
		sess.SetAIPaused(true)
		slog.Info("AI gRPC send PAUSED", "uuid", uuid)
	}
}

// ResumeAI resumes the AI gRPC send for a session.
func (s *Server) ResumeAI(uuid string) {
	if sess, ok := s.GetSession(uuid); ok {
		sess.SetAIPaused(false)
		slog.Info("AI gRPC send RESUMED", "uuid", uuid)
	}
}

// InternalHandler creates an HTTP handler for internal Bridge API.
func (s *Server) InternalHandler() http.Handler {
	mux := http.NewServeMux()

	mux.HandleFunc("/internal/health", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
		w.Write([]byte(`{"status":"healthy"}`))
	})

	mux.HandleFunc("/internal/ai-pause/", func(w http.ResponseWriter, r *http.Request) {
		uuid := extractUUID(r.URL.Path, "/internal/ai-pause/")
		s.PauseAI(uuid)
		w.WriteHeader(http.StatusOK)
	})

	mux.HandleFunc("/internal/ai-resume/", func(w http.ResponseWriter, r *http.Request) {
		uuid := extractUUID(r.URL.Path, "/internal/ai-resume/")
		s.ResumeAI(uuid)
		w.WriteHeader(http.StatusOK)
	})

	// T-19: Shutdown notification from Orchestrator
	mux.HandleFunc("/internal/shutdown", func(w http.ResponseWriter, r *http.Request) {
		slog.Info("Shutdown notification received from Orchestrator")
		w.WriteHeader(http.StatusOK)
		w.Write([]byte(`{"status":"shutting_down"}`))
	})

	mux.HandleFunc("/internal/dtmf/", func(w http.ResponseWriter, r *http.Request) {
		uuid := extractUUID(r.URL.Path, "/internal/dtmf/")

		var body struct {
			Digit string `json:"digit"`
		}
		if err := json.NewDecoder(r.Body).Decode(&body); err != nil || body.Digit == "" {
			http.Error(w, `{"error":"digit required"}`, http.StatusBadRequest)
			return
		}

		if sess, ok := s.GetSession(uuid); ok {
			// T-18: Return error to caller if DTMF forwarding fails
			if err := sess.ForwardDtmf(body.Digit); err != nil {
				http.Error(w, `{"error":"dtmf forward failed"}`, http.StatusInternalServerError)
				return
			}
		}
		w.WriteHeader(http.StatusOK)
	})

	return mux
}

func extractUUID(path, prefix string) string {
	return strings.TrimPrefix(path, prefix)
}
