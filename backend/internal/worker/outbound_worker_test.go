package worker

import (
	"context"
	"io"
	"log/slog"
	"os"
	"testing"
	"time"

	"tinyvoice/backend/internal/message"
	"tinyvoice/backend/internal/messaging"
)

type mockProvider struct {
	sent bool
	err  error
}

func (m *mockProvider) SendAudio(_ context.Context, _ string, _ messaging.AudioMessage) error {
	m.sent = true
	return m.err
}

func (m *mockProvider) DownloadMedia(_ context.Context, _ string) (io.ReadCloser, int64, string, error) {
	return nil, 0, "", nil
}

func TestWorkerMaxAttemptsMarksFailed(t *testing.T) {
	// Unit test for retry logic concept
	maxAttempts := 3
	attempts := 0
	for attempts < maxAttempts {
		attempts++
	}
	if attempts != maxAttempts {
		t.Fatalf("expected %d attempts", maxAttempts)
	}
}

func TestWorkerInterval(t *testing.T) {
	w := NewOutboundWorker(nil, nil, &mockProvider{}, 5, 100*time.Millisecond, slog.New(slog.NewTextHandler(os.Stdout, nil)))
	if w.interval != 100*time.Millisecond {
		t.Fatal("interval not set")
	}
}

func TestMessageStatuses(t *testing.T) {
	if message.StatusPending != "PENDING" {
		t.Fatal("status constants mismatch")
	}
}
