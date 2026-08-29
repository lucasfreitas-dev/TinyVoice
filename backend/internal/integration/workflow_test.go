package integration

import (
	"bytes"
	"context"
	"io"
	"log/slog"
	"os"
	"sync"
	"testing"
	"time"

	"tinyvoice/backend/internal/message"
	"tinyvoice/backend/internal/messaging"
	"tinyvoice/backend/internal/worker"
)

type memStorage struct {
	mu   sync.Mutex
	data map[string][]byte
}

func newMemStorage() *memStorage {
	return &memStorage{data: map[string][]byte{}}
}

func (m *memStorage) EnsureBucket(_ context.Context) error { return nil }

func (m *memStorage) Put(_ context.Context, key string, reader io.Reader, _ int64, _ string) error {
	b, err := io.ReadAll(reader)
	if err != nil {
		return err
	}
	m.mu.Lock()
	m.data[key] = b
	m.mu.Unlock()
	return nil
}

func (m *memStorage) Get(_ context.Context, key string) (io.ReadCloser, int64, error) {
	m.mu.Lock()
	b, ok := m.data[key]
	m.mu.Unlock()
	if !ok {
		return nil, 0, io.EOF
	}
	return io.NopCloser(bytes.NewReader(b)), int64(len(b)), nil
}

func (m *memStorage) PutFile(_ context.Context, key, localPath, _ string) error {
	b, err := os.ReadFile(localPath)
	if err != nil {
		return err
	}
	m.mu.Lock()
	m.data[key] = b
	m.mu.Unlock()
	return nil
}

type mockMessaging struct {
	sent bool
}

func (m *mockMessaging) SendAudio(_ context.Context, _ string, _ messaging.AudioMessage) error {
	m.sent = true
	return nil
}

func (m *mockMessaging) DownloadMedia(_ context.Context, _ string) (io.ReadCloser, int64, string, error) {
	return nil, 0, "", nil
}

func TestUploadToWorkerFlowConcept(t *testing.T) {
	store := newMemStorage()
	provider := &mockMessaging{}

	w := worker.NewOutboundWorker(nil, store, provider, 3, time.Second, slog.New(slog.NewTextHandler(os.Stdout, nil)))
	if w == nil {
		t.Fatal("worker nil")
	}

	// Verify storage round-trip used by worker pipeline
	ctx := context.Background()
	payload := []byte("test wav data")
	if err := store.Put(ctx, "audio/outbound/test-id", bytes.NewReader(payload), int64(len(payload)), "audio/wav"); err != nil {
		t.Fatal(err)
	}
	rc, size, err := store.Get(ctx, "audio/outbound/test-id")
	if err != nil {
		t.Fatal(err)
	}
	defer rc.Close()
	if size != int64(len(payload)) {
		t.Fatalf("expected size %d, got %d", len(payload), size)
	}
}

func TestMessageStatusConstants(t *testing.T) {
	if message.StatusPending != "PENDING" || message.StatusSent != "SENT" {
		t.Fatal("unexpected status values")
	}
}
