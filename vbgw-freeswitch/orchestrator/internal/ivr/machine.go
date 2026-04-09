/**
 * @file machine.go
 * @description IVR 상태머신 — channel 기반 5-state FSM (교착 원천 차단)
 *
 * 변경 이력
 * ─────────────────────────────────────────
 * v1.0.0 | 2026-04-07 | [Implementer] | 최초 생성 | IDLE→MENU→AI_CHAT/TRANSFER/DISCONNECT
 * v1.1.0 | 2026-04-09 | [Implementer] | T-20 | IVR inactivity timeout (default 5min)
 * ─────────────────────────────────────────
 */

package ivr

import (
	"context"
	"log/slog"
	"time"
)

// State represents the IVR FSM state.
type State int

const (
	Idle       State = iota
	Menu             // Main menu — waiting for DTMF
	AiChat           // Connected to AI engine
	Transfer         // Transferring to agent
	Disconnect       // Call ending
)

func (s State) String() string {
	switch s {
	case Idle:
		return "IDLE"
	case Menu:
		return "MENU"
	case AiChat:
		return "AI_CHAT"
	case Transfer:
		return "TRANSFER"
	case Disconnect:
		return "DISCONNECT"
	default:
		return "UNKNOWN"
	}
}

// EventType identifies the IVR event kind.
type EventType int

const (
	DtmfEvent        EventType = iota
	HangupEvent
	ActivateMenuEvent
)

// IvrEvent is sent to the machine's event channel.
type IvrEvent struct {
	Type  EventType
	Digit string // for DtmfEvent: "0"-"9", "*", "#"
}

// Callbacks holds IVR transition action functions.
type Callbacks struct {
	OnEnterAiChat func()
	OnTransfer    func()
	OnDisconnect  func()
	OnRepeatMenu  func()
	OnForwardDtmf func(digit string)
}

// T-20: Default inactivity timeout (no DTMF activity in Menu state)
const defaultInactivityTimeout = 5 * time.Minute

// Machine is a channel-based IVR FSM. Single goroutine, no mutex needed.
type Machine struct {
	sessionID string
	state     State
	EventCh   chan IvrEvent
	cb        Callbacks
}

// NewMachine creates a new IVR machine.
func NewMachine(sessionID string, cb Callbacks) *Machine {
	return &Machine{
		sessionID: sessionID,
		state:     Idle,
		EventCh:   make(chan IvrEvent, 16),
		cb:        cb,
	}
}

// Run processes events until context is cancelled. Must be called as a goroutine.
// T-20: Added inactivity timeout — if no events for 5 minutes, auto-disconnect.
func (m *Machine) Run(ctx context.Context) {
	slog.Info("IVR machine started", "session", m.sessionID, "state", m.state.String())
	inactivityTimer := time.NewTimer(defaultInactivityTimeout)
	defer inactivityTimer.Stop()

	for {
		select {
		case <-ctx.Done():
			slog.Info("IVR machine stopped", "session", m.sessionID)
			return
		case evt := <-m.EventCh:
			m.handleEvent(evt)
			inactivityTimer.Reset(defaultInactivityTimeout)
		case <-inactivityTimer.C:
			slog.Warn("IVR inactivity timeout", "session", m.sessionID, "state", m.state.String())
			if m.cb.OnDisconnect != nil {
				m.cb.OnDisconnect()
			}
			return
		}
	}
}

// State returns the current FSM state.
func (m *Machine) State() State {
	return m.state
}

func (m *Machine) transition(newState State) {
	slog.Info("IVR state transition",
		"session", m.sessionID,
		"from", m.state.String(),
		"to", newState.String(),
	)
	m.state = newState
}

func (m *Machine) handleEvent(evt IvrEvent) {
	switch evt.Type {
	case ActivateMenuEvent:
		m.transition(Menu)
		if m.cb.OnRepeatMenu != nil {
			m.cb.OnRepeatMenu()
		}

	case HangupEvent:
		m.transition(Idle)

	case DtmfEvent:
		m.handleDtmf(evt.Digit)
	}
}

func (m *Machine) handleDtmf(digit string) {
	slog.Info("IVR DTMF received", "session", m.sessionID, "digit", digit, "state", m.state.String())

	switch m.state {
	case Menu:
		switch digit {
		case "1":
			m.transition(AiChat)
			if m.cb.OnEnterAiChat != nil {
				m.cb.OnEnterAiChat()
			}
		case "0":
			m.transition(Transfer)
			if m.cb.OnTransfer != nil {
				m.cb.OnTransfer()
			}
		case "#":
			m.transition(Disconnect)
			if m.cb.OnDisconnect != nil {
				m.cb.OnDisconnect()
			}
		case "*":
			if m.cb.OnRepeatMenu != nil {
				m.cb.OnRepeatMenu()
			}
		default:
			if m.cb.OnForwardDtmf != nil {
				m.cb.OnForwardDtmf(digit)
			}
		}

	case AiChat:
		switch digit {
		case "0":
			m.transition(Transfer)
			if m.cb.OnTransfer != nil {
				m.cb.OnTransfer()
			}
		case "*":
			m.transition(Menu)
			if m.cb.OnRepeatMenu != nil {
				m.cb.OnRepeatMenu()
			}
		case "#":
			m.transition(Disconnect)
			if m.cb.OnDisconnect != nil {
				m.cb.OnDisconnect()
			}
		default:
			slog.Info("IVR: forwarding DTMF to AI", "session", m.sessionID, "digit", digit)
			if m.cb.OnForwardDtmf != nil {
				m.cb.OnForwardDtmf(digit)
			}
		}
	}
}
