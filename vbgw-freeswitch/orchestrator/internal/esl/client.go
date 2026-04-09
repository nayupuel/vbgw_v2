/**
 * @file client.go
 * @description ESL TCP 클라이언트 — FreeSWITCH 이벤트 구독 및 API 명령 디스패치
 *
 * 변경 이력
 * ─────────────────────────────────────────
 * v1.0.0 | 2026-04-07 | [Implementer] | 최초 생성 | ESL TCP 연결, auth, event loop
 * v1.1.0 | 2026-04-09 | [Implementer] | T-02,T-10,T-13 | apiRespCh 직렬화, Sofia ctx, reconnect 직렬화
 * ─────────────────────────────────────────
 */

package esl

import (
	"bufio"
	"context"
	"fmt"
	"log/slog"
	"net"
	"strings"
	"sync"
	"time"
)

// EventHandler is called for each received ESL event.
type EventHandler func(evt *Event)

// Client manages the ESL TCP connection to FreeSWITCH.
type Client struct {
	host     string
	port     int
	password string

	conn    net.Conn
	reader  *bufio.Reader
	mu      sync.Mutex // protects conn writes + API request serialization
	handler EventHandler

	// Q-04: API response channel — eventLoop routes api/response here
	// T-02: Expanded buffer to prevent response drop under reconnect race
	apiRespCh chan string

	connected     bool
	onReconnect   func() // called after successful reconnection
	ctx           context.Context
	cancel        context.CancelFunc
}

// NewClient creates a new ESL client (not yet connected).
func NewClient(host string, port int, password string, handler EventHandler) *Client {
	ctx, cancel := context.WithCancel(context.Background())
	return &Client{
		host:      host,
		port:      port,
		password:  password,
		apiRespCh: make(chan string, 16),
		handler:  handler,
		ctx:      ctx,
		cancel:   cancel,
	}
}

// Connect establishes the ESL TCP connection, authenticates, and subscribes to events.
// R-02: Closes any existing connection first to prevent goroutine overlap.
func (c *Client) Connect() error {
	// R-02: Close existing connection before re-establishing (prevents reader overlap)
	if c.conn != nil {
		c.conn.Close()
		c.conn = nil
	}

	addr := fmt.Sprintf("%s:%d", c.host, c.port)
	slog.Info("ESL connecting", "addr", addr)

	conn, err := net.DialTimeout("tcp", addr, 10*time.Second)
	if err != nil {
		return fmt.Errorf("ESL dial failed: %w", err)
	}

	// Close conn on any subsequent error to prevent fd leak
	success := false
	defer func() {
		if !success {
			conn.Close()
		}
	}()

	c.conn = conn
	c.reader = bufio.NewReader(conn)

	// Read auth/request
	if _, err := c.readResponse(); err != nil {
		return fmt.Errorf("ESL read auth request: %w", err)
	}

	// Send auth
	if err := c.sendCommand(fmt.Sprintf("auth %s", c.password)); err != nil {
		return fmt.Errorf("ESL auth send: %w", err)
	}
	resp, err := c.readResponse()
	if err != nil {
		return fmt.Errorf("ESL auth response: %w", err)
	}
	if !strings.Contains(resp, "+OK") && !strings.Contains(resp, "Reply-Text: +OK") {
		return fmt.Errorf("ESL auth rejected: %s", resp)
	}

	// Subscribe to events
	// P-12: Added CHANNEL_HOLD/CHANNEL_UNHOLD for Re-INVITE hold/resume detection
	events := "CHANNEL_CREATE CHANNEL_ANSWER CHANNEL_HANGUP_COMPLETE " +
		"CHANNEL_PARK DTMF CHANNEL_BRIDGE CHANNEL_UNBRIDGE " +
		"CHANNEL_HOLD CHANNEL_UNHOLD " +
		"CUSTOM sofia::register sofia::unregister"
	if err := c.sendCommand(fmt.Sprintf("event plain %s", events)); err != nil {
		return fmt.Errorf("ESL event subscribe: %w", err)
	}
	if _, err := c.readResponse(); err != nil {
		return fmt.Errorf("ESL event subscribe response: %w", err)
	}

	success = true

	c.connected = true
	slog.Info("ESL connected and subscribed", "addr", addr)

	// Start event reader goroutine
	go c.eventLoop()

	return nil
}

// ConnectWithRetry attempts to connect with exponential backoff.
func (c *Client) ConnectWithRetry(ctx context.Context) error {
	backoff := 1 * time.Second
	maxBackoff := 30 * time.Second

	for {
		err := c.Connect()
		if err == nil {
			return nil
		}
		slog.Warn("ESL connection failed, retrying", "err", err, "backoff", backoff)

		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-time.After(backoff):
		}

		backoff = min(backoff*2, maxBackoff)
	}
}

// SetOnReconnect registers a callback invoked after successful ESL reconnection.
// Use this to sync session state (e.g., reconcile orphan sessions via "show channels").
func (c *Client) SetOnReconnect(fn func()) {
	c.onReconnect = fn
}

// IsConnected returns the connection state.
func (c *Client) IsConnected() bool {
	return c.connected
}

// GetActiveChannelUUIDs queries FS for all active channel UUIDs.
// Used after reconnection to reconcile orphan sessions.
func (c *Client) GetActiveChannelUUIDs() (map[string]bool, error) {
	resp, err := c.SendAPI("show channels")
	if err != nil {
		return nil, err
	}
	uuids := make(map[string]bool)
	for _, line := range strings.Split(resp, "\n") {
		fields := strings.Split(line, ",")
		if len(fields) > 0 {
			uuid := strings.TrimSpace(fields[0])
			if len(uuid) == 36 && strings.Contains(uuid, "-") {
				uuids[uuid] = true
			}
		}
	}
	return uuids, nil
}

// Close shuts down the ESL connection.
func (c *Client) Close() {
	c.cancel()
	c.connected = false
	if c.conn != nil {
		c.conn.Close()
	}
}

// SendAPI sends an ESL API command and waits for the response via apiRespCh.
// Q-04: Uses eventLoop for reading to avoid reader contention.
func (c *Client) SendAPI(command string) (string, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	if err := c.sendCommandLocked(fmt.Sprintf("api %s", command)); err != nil {
		return "", err
	}

	// Wait for eventLoop to route the api/response to apiRespCh
	select {
	case resp := <-c.apiRespCh:
		return resp, nil
	case <-time.After(10 * time.Second):
		return "", fmt.Errorf("ESL API timeout: %s", command)
	case <-c.ctx.Done():
		return "", c.ctx.Err()
	}
}

// SendBgAPI sends an ESL background API command.
func (c *Client) SendBgAPI(command string) (string, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	if err := c.sendCommandLocked(fmt.Sprintf("bgapi %s", command)); err != nil {
		return "", err
	}

	select {
	case resp := <-c.apiRespCh:
		return resp, nil
	case <-time.After(10 * time.Second):
		return "", fmt.Errorf("ESL bgapi timeout: %s", command)
	case <-c.ctx.Done():
		return "", c.ctx.Err()
	}
}

// eventLoop reads events from the ESL connection and dispatches them.
// On disconnect, triggers automatic reconnection.
func (c *Client) eventLoop() {
	defer func() {
		c.connected = false
		slog.Warn("ESL event loop exited")
		// Auto-reconnect unless context is cancelled (shutdown)
		if c.ctx.Err() == nil {
			go c.autoReconnect()
		}
	}()

	for {
		select {
		case <-c.ctx.Done():
			return
		default:
		}

		data, err := c.readEventData()
		if err != nil {
			if c.ctx.Err() != nil {
				return
			}
			slog.Error("ESL read error, will reconnect", "err", err)
			c.connected = false
			return
		}

		if data == "" {
			continue
		}

		// Q-04: Route API responses to apiRespCh instead of event handler
		if strings.Contains(data, "Content-Type: api/response") ||
			strings.Contains(data, "Content-Type: command/reply") {
			// Extract body (after double newline)
			body := ""
			if idx := strings.Index(data, "\n\n"); idx >= 0 {
				body = data[idx+2:]
			}
			select {
			case c.apiRespCh <- body:
			default:
				slog.Error("ESL apiRespCh full, dropping API response — possible concurrent API call leak")
			}
			continue
		}

		evt := ParseEvent(data)
		if evt.Name() != "" && c.handler != nil {
			c.handler(evt)
		}
	}
}

// autoReconnect attempts to re-establish the ESL connection with backoff.
func (c *Client) autoReconnect() {
	backoff := 1 * time.Second
	maxBackoff := 30 * time.Second

	for {
		select {
		case <-c.ctx.Done():
			return
		case <-time.After(backoff):
		}

		slog.Info("ESL auto-reconnecting...", "backoff", backoff)
		if err := c.Connect(); err != nil {
			slog.Warn("ESL reconnect failed", "err", err, "next_backoff", min(backoff*2, maxBackoff))
			backoff = min(backoff*2, maxBackoff)
			continue
		}

		slog.Info("ESL reconnected successfully")
		if c.onReconnect != nil {
			c.onReconnect()
		}
		return
	}
}

// readEventData reads a complete ESL event (headers + body).
func (c *Client) readEventData() (string, error) {
	var headers strings.Builder

	for {
		line, err := c.reader.ReadString('\n')
		if err != nil {
			return "", err
		}
		line = strings.TrimRight(line, "\r\n")

		if line == "" {
			break
		}
		headers.WriteString(line)
		headers.WriteString("\n")
	}

	headerStr := headers.String()
	if headerStr == "" {
		return "", nil
	}

	// Check for Content-Length to read body
	var contentLength int
	for _, line := range strings.Split(headerStr, "\n") {
		if strings.HasPrefix(line, "Content-Length: ") {
			fmt.Sscanf(line, "Content-Length: %d", &contentLength)
		}
	}

	if contentLength > 0 {
		body := make([]byte, contentLength)
		_, err := c.reader.Read(body)
		if err != nil {
			return "", err
		}
		return headerStr + "\n" + string(body), nil
	}

	return headerStr, nil
}

func (c *Client) sendCommand(cmd string) error {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.sendCommandLocked(cmd)
}

func (c *Client) sendCommandLocked(cmd string) error {
	_, err := fmt.Fprintf(c.conn, "%s\n\n", cmd)
	return err
}

func (c *Client) readResponse() (string, error) {
	var resp strings.Builder
	for {
		line, err := c.reader.ReadString('\n')
		if err != nil {
			return resp.String(), err
		}
		line = strings.TrimRight(line, "\r\n")
		if line == "" {
			break
		}
		resp.WriteString(line)
		resp.WriteString("\n")
	}
	return resp.String(), nil
}
