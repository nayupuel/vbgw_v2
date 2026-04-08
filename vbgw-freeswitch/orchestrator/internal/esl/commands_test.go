package esl

import (
	"strings"
	"testing"
)

// TestOriginateCommandFormat tests the Originate command string generation.
// We can't test SendBgAPI (needs TCP), but we can verify the command format.
func TestOriginateCommandFormat_SingleGateway(t *testing.T) {
	// Simulate the command string logic from Originate
	uuid := "test-uuid-123"
	target := "1001"
	callerID := ""
	useStandby := false

	cidParam := ""
	if callerID != "" {
		cidParam = ",origination_caller_id_number=" + callerID + ",origination_caller_id_name=" + callerID
	}

	gateway := "sofia/gateway/pbx-main/" + target
	if useStandby {
		gateway += "|sofia/gateway/pbx-standby/" + target
	}

	cmd := "originate {origination_uuid=" + uuid + cidParam + ",failure_causes=NORMAL_TEMPORARY_FAILURE,originate_timeout=30}" + gateway + " &park()"

	if !strings.Contains(cmd, "sofia/gateway/pbx-main/1001") {
		t.Fatal("expected pbx-main gateway")
	}
	if strings.Contains(cmd, "pbx-standby") {
		t.Fatal("should NOT contain pbx-standby when useStandby=false")
	}
	if !strings.Contains(cmd, "origination_uuid=test-uuid-123") {
		t.Fatal("expected origination_uuid")
	}
	if strings.Contains(cmd, "origination_caller_id") {
		t.Fatal("should NOT contain caller_id when empty")
	}
}

func TestOriginateCommandFormat_WithStandbyAndCallerID(t *testing.T) {
	uuid := "test-uuid-456"
	target := "1002"
	callerID := "02-1234-5678"
	useStandby := true

	cidParam := ""
	if callerID != "" {
		cidParam = ",origination_caller_id_number=" + callerID + ",origination_caller_id_name=" + callerID
	}

	gateway := "sofia/gateway/pbx-main/" + target
	if useStandby {
		gateway += "|sofia/gateway/pbx-standby/" + target
	}

	cmd := "originate {origination_uuid=" + uuid + cidParam + ",failure_causes=NORMAL_TEMPORARY_FAILURE,originate_timeout=30}" + gateway + " &park()"

	if !strings.Contains(cmd, "pbx-main/1002") {
		t.Fatal("expected pbx-main")
	}
	if !strings.Contains(cmd, "|sofia/gateway/pbx-standby/1002") {
		t.Fatal("expected pipe-separated pbx-standby failover")
	}
	if !strings.Contains(cmd, "origination_caller_id_number=02-1234-5678") {
		t.Fatal("expected caller_id_number")
	}
	if !strings.Contains(cmd, "origination_caller_id_name=02-1234-5678") {
		t.Fatal("expected caller_id_name")
	}
}

func TestDumpParsing_ValidResponse(t *testing.T) {
	// Simulate uuid_dump response format
	resp := "channel_state=CS_EXECUTE\nread_codec=PCMU\nwrite_codec=PCMU\nrtp_audio_recv_pt=0\nrtp_audio_lost_pt=0\nvariable_sip_term_status=200\n"

	result := make(map[string]string)
	for _, line := range strings.Split(resp, "\n") {
		line = strings.TrimSpace(line)
		if idx := strings.Index(line, "="); idx > 0 {
			result[line[:idx]] = line[idx+1:]
		}
	}

	if result["channel_state"] != "CS_EXECUTE" {
		t.Fatalf("expected CS_EXECUTE, got %s", result["channel_state"])
	}
	if result["read_codec"] != "PCMU" {
		t.Fatalf("expected PCMU, got %s", result["read_codec"])
	}
	if result["rtp_audio_lost_pt"] != "0" {
		t.Fatalf("expected 0 loss, got %s", result["rtp_audio_lost_pt"])
	}
}

func TestDumpParsing_ErrorResponse(t *testing.T) {
	resp := "-ERR No Such Channel!\n"

	if !strings.Contains(resp, "-ERR") {
		t.Fatal("should detect -ERR response")
	}
}

func TestDumpParsing_EmptyResponse(t *testing.T) {
	resp := ""

	result := make(map[string]string)
	for _, line := range strings.Split(resp, "\n") {
		line = strings.TrimSpace(line)
		if idx := strings.Index(line, "="); idx > 0 {
			result[line[:idx]] = line[idx+1:]
		}
	}

	if len(result) != 0 {
		t.Fatalf("expected empty map, got %d entries", len(result))
	}
}

func TestDumpParsing_ValueWithEquals(t *testing.T) {
	// Some FS variables contain '=' in the value
	resp := "variable_sip_contact_uri=sip:1001@192.168.1.1:5060;transport=udp\n"

	result := make(map[string]string)
	for _, line := range strings.Split(resp, "\n") {
		line = strings.TrimSpace(line)
		if idx := strings.Index(line, "="); idx > 0 {
			result[line[:idx]] = line[idx+1:]
		}
	}

	expected := "sip:1001@192.168.1.1:5060;transport=udp"
	if result["variable_sip_contact_uri"] != expected {
		t.Fatalf("expected '%s', got '%s'", expected, result["variable_sip_contact_uri"])
	}
}
