package grpc

import (
	"testing"

	"google.golang.org/grpc/codes"
)

func TestPermanentCodes_Contains7Codes(t *testing.T) {
	expected := []codes.Code{
		codes.InvalidArgument,
		codes.PermissionDenied,
		codes.Unauthenticated,
		codes.FailedPrecondition,
		codes.Unimplemented,
		codes.Internal,
		codes.DataLoss,
	}

	if len(permanentCodes) != len(expected) {
		t.Fatalf("expected %d permanent codes, got %d", len(expected), len(permanentCodes))
	}

	for _, code := range expected {
		if !permanentCodes[code] {
			t.Errorf("expected %s to be permanent", code)
		}
	}
}

func TestPermanentCodes_RetryableCodes(t *testing.T) {
	// These codes SHOULD be retried (not in permanentCodes)
	retryable := []codes.Code{
		codes.Unavailable,          // Server temporarily unavailable
		codes.DeadlineExceeded,     // Timeout
		codes.Aborted,              // Transaction conflict
		codes.ResourceExhausted,    // Rate limited
		codes.Canceled,             // Client cancelled
		codes.Unknown,              // Unknown error
	}

	for _, code := range retryable {
		if permanentCodes[code] {
			t.Errorf("%s should be retryable, but is marked as permanent", code)
		}
	}
}

func TestPermanentCodes_UnavailableIsRetryable(t *testing.T) {
	// This is the most important case: UNAVAILABLE means "try again later"
	// If this were marked permanent, gRPC reconnection would be broken
	if permanentCodes[codes.Unavailable] {
		t.Fatal("codes.Unavailable MUST be retryable (not permanent)")
	}
}
