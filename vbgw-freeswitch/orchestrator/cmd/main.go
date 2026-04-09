/**
 * @file main.go
 * @description Orchestrator 진입점 — DI + 5-stage graceful shutdown
 *
 * 변경 이력
 * ─────────────────────────────────────────
 * v1.0.0 | 2026-04-07 | [Implementer] | 최초 생성 | ESL 연결, HTTP API, 5단계 셧다운
 * v1.1.0 | 2026-04-09 | [Implementer] | T-03~T-13,T-19~T-20 | 28개 이슈 수정
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
	"strings"
	"syscall"
	"time"

	"vbgw-orchestrator/internal/api"
	"vbgw-orchestrator/internal/cdr"
	"vbgw-orchestrator/internal/config"
	"vbgw-orchestrator/internal/esl"
	"vbgw-orchestrator/internal/ivr"
	"vbgw-orchestrator/internal/metrics"
	"vbgw-orchestrator/internal/recording"
	"vbgw-orchestrator/internal/session"

	"github.com/google/uuid"
)

func main() {
	// Load config
	cfg := config.Load()
	setupLogging(cfg.LogLevel)

	slog.Info("Orchestrator starting",
		"profile", cfg.RuntimeProfile,
		"max_sessions", cfg.MaxSessions,
		"http_port", cfg.HTTPPort,
	)

	// Production security validation
	if cfg.RuntimeProfile == "production" {
		validateProdConfig(cfg)
	}

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	// Initialize session manager
	sessionMgr := session.NewManager(cfg.MaxSessions)

	// Connect ESL (eslClient used in handler closure, declared first)
	var eslClient *esl.Client
	eslHandler := func(evt *esl.Event) {
		handleESLEvent(evt, sessionMgr, cfg, eslClient)
	}
	eslClient = esl.NewClient(cfg.ESLHost, cfg.ESLPort, cfg.ESLPassword, eslHandler)

	// Register reconnect callback: reconcile orphan sessions after ESL reconnection
	eslClient.SetOnReconnect(func() {
		metrics.ESLConnected.Set(1)
		slog.Info("ESL reconnected — syncing active sessions")
		activeUUIDs, err := eslClient.GetActiveChannelUUIDs()
		if err != nil {
			slog.Error("Failed to get active channels after reconnect", "err", err)
			return
		}
		// Release sessions that no longer exist in FreeSWITCH
		orphanCount := 0
		sessionMgr.ForEach(func(s *session.SessionState) {
			if !activeUUIDs[s.FSUUID] {
				slog.Warn("Releasing orphan session", "session_id", s.SessionID, "fs_uuid", s.FSUUID)
				sessionMgr.Release(s.SessionID)
				orphanCount++
			}
		})
		if orphanCount > 0 {
			slog.Info("Orphan session cleanup complete", "released", orphanCount)
		}
		metrics.ActiveCalls.Set(float64(sessionMgr.Count()))
	})

	if err := eslClient.ConnectWithRetry(ctx); err != nil {
		slog.Error("ESL connection failed", "err", err)
		os.Exit(1)
	}
	metrics.ESLConnected.Set(1)

	// Q-09: Ensure FS is not stuck in paused state from previous Orchestrator shutdown
	if err := eslClient.Resume(); err != nil {
		slog.Warn("fsctl resume failed (may be normal on fresh start)", "err", err)
	}

	// Start recording cleaner
	cleaner := recording.NewCleaner(
		cfg.RecordingDir, cfg.RecordingMaxDays, cfg.RecordingMaxMB, cfg.RecordingEnable,
	)
	go cleaner.Run(ctx)

	// P-15 + R-04 + S-03: Sofia gateway health monitor with consecutive failure alarm
	go func() {
		ticker := time.NewTicker(30 * time.Second)
		defer ticker.Stop()

		const alarmThreshold = 6 // 6 consecutive failures = 3 minutes
		consecutiveFails := 0

		for {
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
				registered := false

				if !eslClient.IsConnected() {
					metrics.SipRegistered.Set(0)
				} else {
					// T-10: Use context-aware timeout to prevent goroutine leak on shutdown
					sofiaCtx, sofiaCancel := context.WithTimeout(ctx, 5*time.Second)
					resultCh := make(chan string, 1)
					go func() {
						resp, err := eslClient.SendAPI("sofia status gateway pbx-main")
						if err != nil {
							resultCh <- ""
							return
						}
						resultCh <- resp
					}()

					select {
					case resp := <-resultCh:
						if resp != "" && strings.Contains(resp, "REGED") {
							registered = true
							metrics.SipRegistered.Set(1)
						} else {
							metrics.SipRegistered.Set(0)
							if resp != "" {
								slog.Warn("PBX gateway not registered", "status", strings.TrimSpace(resp))
							}
						}
					case <-sofiaCtx.Done():
						if ctx.Err() != nil {
							sofiaCancel()
							return
						}
						slog.Warn("Sofia status check timeout (5s)")
						metrics.SipRegistered.Set(0)
					}
					sofiaCancel()
				}

				// S-03: Consecutive failure alarm logic
				if registered {
					if consecutiveFails >= alarmThreshold {
						slog.Info("ALARM CLEARED: PBX gateway re-registered",
							"was_down_checks", consecutiveFails)
					}
					consecutiveFails = 0
					metrics.SipRegistrationAlarm.Set(0)
				} else {
					consecutiveFails++
					if consecutiveFails == alarmThreshold {
						slog.Error("ALARM: PBX gateway registration lost for 3+ minutes",
							"consecutive_fails", consecutiveFails,
							"action", "All outbound calls will fail. Check PBX connectivity.")
						metrics.SipRegistrationAlarm.Set(1)
					} else if consecutiveFails > alarmThreshold && consecutiveFails%10 == 0 {
						slog.Error("ALARM ONGOING: PBX gateway still unregistered",
							"consecutive_fails", consecutiveFails)
					}
				}
			}
		}
	}()

	// HTTP server
	router := api.NewRouter(cfg, eslClient, sessionMgr)
	httpServer := &http.Server{
		Addr:    fmt.Sprintf(":%d", cfg.HTTPPort),
		Handler: router,
	}

	go func() {
		slog.Info("HTTP API listening", "port", cfg.HTTPPort)
		if err := httpServer.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			slog.Error("HTTP server error", "err", err)
			os.Exit(1)
		}
	}()

	// Graceful shutdown
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)

	<-sigCh
	slog.Info("[Shutdown 1/5] HTTP server: rejecting new requests")
	httpServer.SetKeepAlivesEnabled(false)
	shutdownCtx, shutdownCancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer shutdownCancel()
	httpServer.Shutdown(shutdownCtx)

	// T-19: Notify Bridge to prepare for shutdown (stop accepting new streams)
	bridgeURL := fmt.Sprintf("http://%s:%d", cfg.BridgeHost, cfg.BridgeInternalPort)
	shutdownBridgeReq, _ := http.NewRequest("POST", bridgeURL+"/internal/shutdown", nil)
	shutdownBridgeClient := &http.Client{Timeout: 5 * time.Second}
	if resp, err := shutdownBridgeClient.Do(shutdownBridgeReq); err != nil {
		slog.Warn("Bridge shutdown notification failed (may be already down)", "err", err)
	} else {
		resp.Body.Close()
	}

	slog.Info("[Shutdown 2/5] ESL: fsctl pause sent")
	eslClient.Pause()

	remaining := sessionMgr.Count()
	slog.Info("[Shutdown 3/5] Draining active sessions", "count", remaining, "timeout", "30s")
	drainCtx, drainCancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer drainCancel()
	// P-04: Kill FS channels on drain timeout to send BYE to PBX
	sessionMgr.WaitAllDrained(drainCtx, func(fsUUID string) {
		slog.Info("Killing channel during shutdown", "fs_uuid", fsUUID)
		eslClient.Kill(fsUUID)
	})

	slog.Info("[Shutdown 4/5] Bridge gRPC streams closed")

	slog.Info("[Shutdown 5/5] ESL connection closed")
	eslClient.Close()
	metrics.ESLConnected.Set(0)

	cancel()
	slog.Info("Orchestrator shutdown complete")
}

// handleESLEvent dispatches incoming ESL events to the appropriate handler.
func handleESLEvent(evt *esl.Event, sessionMgr *session.Manager, cfg *config.Config, eslClient *esl.Client) {
	switch evt.Name() {
	case "CHANNEL_CREATE":
		onChannelCreate(evt, sessionMgr, eslClient)

	case "CHANNEL_ANSWER":
		onChannelAnswer(evt, sessionMgr)

	case "CHANNEL_PARK":
		onChannelPark(evt, sessionMgr, eslClient, cfg)

	case "DTMF":
		onDtmf(evt, sessionMgr)

	case "CHANNEL_HANGUP_COMPLETE":
		onChannelHangup(evt, sessionMgr)

	// P-12: SIP Hold/Resume (Re-INVITE) → pause/resume AI streaming
	case "CHANNEL_HOLD":
		onChannelHold(evt, sessionMgr, cfg)

	case "CHANNEL_UNHOLD":
		onChannelUnhold(evt, sessionMgr, cfg)

	case "CUSTOM":
		switch evt.SubClass() {
		case "sofia::register":
			slog.Info("SIP registered")
			metrics.SipRegistered.Set(1)
		case "sofia::unregister":
			slog.Info("SIP unregistered")
			metrics.SipRegistered.Set(0)
		}
	}
}

func onChannelCreate(evt *esl.Event, sessionMgr *session.Manager, eslClient *esl.Client) {
	fsUUID := evt.UUID()
	slog.Info("CHANNEL_CREATE", "fs_uuid", fsUUID, "caller_id", evt.CallerID())

	// Check if this is our originated call (UUID already in sessions)
	if _, exists := sessionMgr.GetByFSUUID(fsUUID); exists {
		return
	}

	// Inbound call — atomic capacity check + session creation
	sessionID := uuid.New().String()
	s := session.NewSession(sessionID, fsUUID, evt.CallerID(), evt.DestNumber())
	if !sessionMgr.AddIfUnderCapacity(s) {
		slog.Warn("Session capacity exceeded, rejecting inbound call", "fs_uuid", fsUUID)
		s.Cancel()
		// P-03: Actively kill the FS channel to send BYE to PBX (prevents zombie park)
		if err := eslClient.Kill(fsUUID); err != nil {
			slog.Error("Failed to kill over-capacity channel", "fs_uuid", fsUUID, "err", err)
		}
		return
	}
	metrics.ActiveCalls.Set(float64(sessionMgr.Count()))

	slog.Info("Session created", "session_id", sessionID, "fs_uuid", fsUUID)
}

func onChannelAnswer(evt *esl.Event, sessionMgr *session.Manager) {
	s, ok := sessionMgr.GetByFSUUID(evt.UUID())
	if !ok {
		return
	}
	now := time.Now()
	s.SetAnsweredAt(now)

	// S-02: Record PDD (Post Dial Delay) — time from CREATE to ANSWER
	pdd := now.Sub(s.CreatedAt).Seconds()
	metrics.CallSetupDuration.Observe(pdd)

	slog.Info("CHANNEL_ANSWER", "session_id", s.SessionID, "pdd_ms", int(pdd*1000))
}

func onChannelPark(evt *esl.Event, sessionMgr *session.Manager, eslClient *esl.Client, cfg *config.Config) {
	s, ok := sessionMgr.GetByFSUUID(evt.UUID())
	if !ok {
		return
	}

	// Start IVR machine
	ivrMachine := ivr.NewMachine(s.SessionID, ivr.Callbacks{
		OnRepeatMenu:  func() { slog.Info("IVR: menu repeat", "session", s.SessionID) },
		OnEnterAiChat: func() { slog.Info("IVR: entering AI chat", "session", s.SessionID) },
		OnTransfer: func() {
			slog.Info("IVR: transfer requested", "session", s.SessionID)
			// R-07: 실제 상담원 전환 실행
			if cfg.IVRTransferTarget != "" {
				if err := eslClient.Transfer(s.FSUUID, cfg.IVRTransferTarget); err != nil {
					slog.Error("IVR transfer failed", "session", s.SessionID, "target", cfg.IVRTransferTarget, "err", err)
				} else {
					slog.Info("IVR: transferred to agent", "session", s.SessionID, "target", cfg.IVRTransferTarget)
				}
			} else {
				slog.Warn("IVR: transfer target not configured (IVR_TRANSFER_TARGET env)", "session", s.SessionID)
			}
		},
		OnDisconnect: func() {
			slog.Info("IVR: disconnect requested", "session", s.SessionID)
			eslClient.Kill(s.FSUUID)
		},
		OnForwardDtmf: func(digit string) {
			slog.Info("IVR: DTMF forwarded to AI", "session", s.SessionID, "digit", digit)
		},
	})

	// P-02: Store IVR event channel in session for DTMF routing
	s.IvrEventCh = make(chan any, 16)
	go func() {
		// T-03: Bridge IvrEventCh → ivrMachine.EventCh with context-aware send
		for {
			select {
			case <-s.Ctx.Done():
				return
			case evt, ok := <-s.IvrEventCh:
				if !ok {
					return
				}
				if ivrEvt, ok := evt.(ivr.IvrEvent); ok {
					select {
					case ivrMachine.EventCh <- ivrEvt:
					case <-s.Ctx.Done():
						return
					}
				}
			}
		}
	}()

	go ivrMachine.Run(s.Ctx)

	// Activate menu
	ivrMachine.EventCh <- ivr.IvrEvent{Type: ivr.ActivateMenuEvent}

	slog.Info("CHANNEL_PARK — IVR started", "session_id", s.SessionID)
}

func onDtmf(evt *esl.Event, sessionMgr *session.Manager) {
	s, ok := sessionMgr.GetByFSUUID(evt.UUID())
	if !ok {
		return
	}
	digit := evt.DtmfDigit()
	slog.Info("DTMF received", "session_id", s.SessionID, "digit", digit)

	// P-02: Forward DTMF to IVR state machine
	// T-03: Use session context to avoid sending to closed channel after Release()
	if s.IvrEventCh != nil {
		select {
		case <-s.Ctx.Done():
			slog.Warn("DTMF dropped: session context cancelled", "session", s.SessionID, "digit", digit)
		case s.IvrEventCh <- ivr.IvrEvent{Type: ivr.DtmfEvent, Digit: digit}:
		default:
			slog.Warn("IVR event channel full, DTMF dropped", "session", s.SessionID, "digit", digit)
		}
	}
}

func onChannelHangup(evt *esl.Event, sessionMgr *session.Manager) {
	s, ok := sessionMgr.GetByFSUUID(evt.UUID())
	if !ok {
		return
	}

	hangupCause := evt.HangupCause()
	sipCode := evt.SipTermStatus()
	cdr.LogHangup(s, hangupCause)
	sessionMgr.Release(s.SessionID)
	metrics.ActiveCalls.Set(float64(sessionMgr.Count()))

	// S-01: SIP hangup cause + code breakdown metric
	if sipCode == "" {
		sipCode = "unknown"
	}
	if hangupCause == "" {
		hangupCause = "UNKNOWN"
	}
	metrics.CallHangupTotal.WithLabelValues(hangupCause, sipCode).Inc()

	slog.Info("CHANNEL_HANGUP", "session_id", s.SessionID, "cause", hangupCause, "sip_code", sipCode)
}

// P-12: Pause AI streaming when PBX puts call on hold (Re-INVITE sendonly)
func onChannelHold(evt *esl.Event, sessionMgr *session.Manager, cfg *config.Config) {
	s, ok := sessionMgr.GetByFSUUID(evt.UUID())
	if !ok {
		return
	}
	s.SetAIPaused(true)
	slog.Info("CHANNEL_HOLD — AI paused (PBX hold)", "session_id", s.SessionID)

	// Notify Bridge to pause gRPC send
	bridgeURL := fmt.Sprintf("http://%s:%d", cfg.BridgeHost, cfg.BridgeInternalPort)
	notifyBridgeHold(bridgeURL, "ai-pause", s.FSUUID)
}

// P-12: Resume AI streaming when PBX takes call off hold
func onChannelUnhold(evt *esl.Event, sessionMgr *session.Manager, cfg *config.Config) {
	s, ok := sessionMgr.GetByFSUUID(evt.UUID())
	if !ok {
		return
	}
	s.SetAIPaused(false)
	slog.Info("CHANNEL_UNHOLD — AI resumed (PBX resume)", "session_id", s.SessionID)

	bridgeURL := fmt.Sprintf("http://%s:%d", cfg.BridgeHost, cfg.BridgeInternalPort)
	notifyBridgeHold(bridgeURL, "ai-resume", s.FSUUID)
}

func notifyBridgeHold(bridgeURL, action, uuid string) {
	url := fmt.Sprintf("%s/internal/%s/%s", bridgeURL, action, uuid)
	// R-06: Content-Type 명시 (일부 HTTP 프레임워크 호환성)
	resp, err := http.Post(url, "application/json", nil)
	if err != nil {
		slog.Error("Bridge hold notification failed", "action", action, "uuid", uuid, "err", err)
		return
	}
	resp.Body.Close()
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

func validateProdConfig(cfg *config.Config) {
	failed := false

	if len(cfg.AdminAPIKey) < 32 {
		slog.Error("Production: ADMIN_API_KEY must be at least 32 characters", "current_len", len(cfg.AdminAPIKey))
		failed = true
	}
	if cfg.AdminAPIKey == "changeme-admin-key" || strings.Contains(cfg.AdminAPIKey, "changeme") {
		slog.Error("Production: ADMIN_API_KEY must not contain default value 'changeme'")
		failed = true
	}
	if cfg.ESLPassword == "ClueCon" || cfg.ESLPassword == "" {
		slog.Error("Production: ESL_PASSWORD must not be default 'ClueCon' or empty")
		failed = true
	}

	if failed {
		slog.Error("Production security validation FAILED — refusing to start")
		os.Exit(1)
	}
	slog.Info("Production security validation passed")
}
