/**
 * @file control.go
 * @description 콜 제어 엔드포인트 — DTMF, transfer, record, bridge/unbridge
 *
 * 변경 이력
 * ─────────────────────────────────────────
 * v1.0.0 | 2026-04-07 | [Implementer] | 최초 생성 | 6개 제어 엔드포인트
 * v1.1.0 | 2026-04-09 | [Implementer] | T-03,T-11,T-18 | IvrEventCh ctx guard, body close, RecordStart UUID검증
 * ─────────────────────────────────────────
 */

package api

import (
	"encoding/json"
	"fmt"
	"log/slog"
	"net/http"
	"regexp"

	"vbgw-orchestrator/internal/esl"
	"vbgw-orchestrator/internal/ivr"
	"vbgw-orchestrator/internal/session"

	"github.com/go-chi/chi/v5"
)

// Input validation patterns (ESL injection prevention)
var (
	dtmfPattern     = regexp.MustCompile(`^[0-9*#A-D]{1,20}$`)
	sipTargetPattern = regexp.MustCompile(`^[a-zA-Z0-9@._:\-/]{1,256}$`)
)

// ControlHandler handles call control endpoints.
type ControlHandler struct {
	ESL        *esl.Client
	Sessions   *session.Manager
	BridgeURL  string
	httpClient *http.Client
}

type dtmfRequest struct {
	Digits string `json:"digits"`
}

type transferRequest struct {
	Target string `json:"target"`
}

type recordRequest struct {
	Path string `json:"path"`
}

type bridgeRequest struct {
	CallID1 string `json:"call_id_1"`
	CallID2 string `json:"call_id_2"`
}

// SendDtmf handles POST /api/v1/calls/{id}/dtmf.
func (h *ControlHandler) SendDtmf(w http.ResponseWriter, r *http.Request) {
	callID := chi.URLParam(r, "id")
	s, ok := h.Sessions.Get(callID)
	if !ok {
		http.Error(w, `{"error":"session not found"}`, http.StatusNotFound)
		return
	}

	var req dtmfRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.Digits == "" {
		http.Error(w, `{"error":"digits required"}`, http.StatusBadRequest)
		return
	}
	if !dtmfPattern.MatchString(req.Digits) {
		http.Error(w, `{"error":"invalid digits format (allowed: 0-9*#A-D, max 20)"}`, http.StatusBadRequest)
		return
	}

	if err := h.ESL.SendDtmf(s.FSUUID, req.Digits); err != nil {
		slog.Error("SendDtmf failed", "err", err)
		http.Error(w, `{"error":"dtmf send failed"}`, http.StatusInternalServerError)
		return
	}

	w.WriteHeader(http.StatusOK)
	fmt.Fprint(w, `{"status":"sent"}`)
}

// Transfer handles POST /api/v1/calls/{id}/transfer.
func (h *ControlHandler) Transfer(w http.ResponseWriter, r *http.Request) {
	callID := chi.URLParam(r, "id")
	s, ok := h.Sessions.Get(callID)
	if !ok {
		http.Error(w, `{"error":"session not found"}`, http.StatusNotFound)
		return
	}

	var req transferRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.Target == "" {
		http.Error(w, `{"error":"target required"}`, http.StatusBadRequest)
		return
	}
	if !sipTargetPattern.MatchString(req.Target) {
		http.Error(w, `{"error":"invalid target format"}`, http.StatusBadRequest)
		return
	}

	if err := h.ESL.Transfer(s.FSUUID, req.Target); err != nil {
		slog.Error("Transfer failed", "err", err)
		http.Error(w, `{"error":"transfer failed"}`, http.StatusInternalServerError)
		return
	}

	// Q-06: Send HangupEvent to IVR to clean up state after transfer
	// T-03: Guard with session context to avoid sending to closed channel
	if s.IvrEventCh != nil {
		select {
		case <-s.Ctx.Done():
		case s.IvrEventCh <- ivr.IvrEvent{Type: ivr.HangupEvent}:
		default:
		}
	}

	w.WriteHeader(http.StatusOK)
	fmt.Fprint(w, `{"status":"transferred"}`)
}

// RecordStart handles POST /api/v1/calls/{id}/record/start.
func (h *ControlHandler) RecordStart(w http.ResponseWriter, r *http.Request) {
	callID := chi.URLParam(r, "id")
	s, ok := h.Sessions.Get(callID)
	if !ok {
		http.Error(w, `{"error":"session not found"}`, http.StatusNotFound)
		return
	}

	path := fmt.Sprintf("/recordings/%s.wav", callID)
	if err := h.ESL.RecordStart(s.FSUUID, path); err != nil {
		slog.Error("RecordStart failed", "err", err)
		http.Error(w, `{"error":"record start failed"}`, http.StatusInternalServerError)
		return
	}

	s.SetRecordPath(path)
	w.WriteHeader(http.StatusOK)
	json.NewEncoder(w).Encode(map[string]string{"status": "recording", "path": path})
}

// RecordStop handles POST /api/v1/calls/{id}/record/stop.
func (h *ControlHandler) RecordStop(w http.ResponseWriter, r *http.Request) {
	callID := chi.URLParam(r, "id")
	s, ok := h.Sessions.Get(callID)
	if !ok {
		http.Error(w, `{"error":"session not found"}`, http.StatusNotFound)
		return
	}

	if err := h.ESL.RecordStop(s.FSUUID); err != nil {
		slog.Error("RecordStop failed", "err", err)
		http.Error(w, `{"error":"record stop failed"}`, http.StatusInternalServerError)
		return
	}

	s.SetRecordPath("")
	w.WriteHeader(http.StatusOK)
	fmt.Fprint(w, `{"status":"stopped"}`)
}

// BridgeCalls handles POST /api/v1/calls/bridge.
func (h *ControlHandler) BridgeCalls(w http.ResponseWriter, r *http.Request) {
	var req bridgeRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, `{"error":"invalid request"}`, http.StatusBadRequest)
		return
	}

	sA, okA := h.Sessions.Get(req.CallID1)
	sB, okB := h.Sessions.Get(req.CallID2)
	if !okA || !okB {
		http.Error(w, `{"error":"one or both sessions not found"}`, http.StatusNotFound)
		return
	}

	// Pause AI for call A
	sA.SetAIPaused(true)
	h.notifyBridge("ai-pause", sA.FSUUID)

	// Bridge via ESL
	if err := h.ESL.Bridge(sA.FSUUID, sB.FSUUID); err != nil {
		sA.SetAIPaused(false)
		slog.Error("Bridge failed", "err", err)
		http.Error(w, `{"error":"bridge failed"}`, http.StatusInternalServerError)
		return
	}

	sA.SetBridgedWith(sB.SessionID)
	sB.SetBridgedWith(sA.SessionID)

	w.WriteHeader(http.StatusOK)
	fmt.Fprint(w, `{"status":"bridged"}`)
}

// UnbridgeCalls handles POST /api/v1/calls/unbridge.
func (h *ControlHandler) UnbridgeCalls(w http.ResponseWriter, r *http.Request) {
	var req bridgeRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, `{"error":"invalid request"}`, http.StatusBadRequest)
		return
	}

	sA, okA := h.Sessions.Get(req.CallID1)
	sB, okB := h.Sessions.Get(req.CallID2)
	if !okA || !okB {
		http.Error(w, `{"error":"one or both sessions not found"}`, http.StatusNotFound)
		return
	}

	// Unbridge via ESL (park)
	if err := h.ESL.Unbridge(sA.FSUUID); err != nil {
		slog.Error("Unbridge failed", "err", err)
		http.Error(w, `{"error":"unbridge failed"}`, http.StatusInternalServerError)
		return
	}

	// Resume AI for call A
	sA.SetAIPaused(false)
	sA.SetBridgedWith("")
	sB.SetBridgedWith("")
	h.notifyBridge("ai-resume", sA.FSUUID)

	w.WriteHeader(http.StatusOK)
	fmt.Fprint(w, `{"status":"unbridged"}`)
}

// BargeIn handles POST /internal/barge-in/{uuid} from Bridge.
func (h *ControlHandler) BargeIn(w http.ResponseWriter, r *http.Request) {
	fsUUID := chi.URLParam(r, "uuid")
	slog.Info("Barge-in request received", "fs_uuid", fsUUID)

	if err := h.ESL.Break(fsUUID); err != nil {
		slog.Error("uuid_break failed", "err", err)
		http.Error(w, `{"error":"break failed"}`, http.StatusInternalServerError)
		return
	}

	slog.Info("uuid_break sent", "fs_uuid", fsUUID)
	w.WriteHeader(http.StatusOK)
	fmt.Fprint(w, `{"status":"break_sent"}`)
}

func (h *ControlHandler) notifyBridge(action, uuid string) {
	url := fmt.Sprintf("%s/internal/%s/%s", h.BridgeURL, action, uuid)
	req, _ := http.NewRequest("POST", url, nil)
	resp, err := h.httpClient.Do(req)
	if err != nil {
		slog.Error("Bridge notification failed", "action", action, "uuid", uuid, "err", err)
		return
	}
	resp.Body.Close()
}
