package message

import (
	"context"
	"testing"
)

type webhookRepoStub struct {
	seen map[string]bool
}

func (s *webhookRepoStub) RecordWebhookEvent(_ context.Context, provider, externalID string) (bool, error) {
	if s.seen[externalID] {
		return false, nil
	}
	s.seen[externalID] = true
	return true, nil
}

func TestWebhookIdempotency(t *testing.T) {
	stub := &webhookRepoStub{seen: map[string]bool{}}

	first, err := stub.RecordWebhookEvent(context.Background(), "evolution", "msg-123")
	if err != nil || !first {
		t.Fatal("first event should be new")
	}

	second, err := stub.RecordWebhookEvent(context.Background(), "evolution", "msg-123")
	if err != nil || second {
		t.Fatal("duplicate event should not be new")
	}
}
