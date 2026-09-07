package conversation

import (
	"context"
	"testing"
)

func TestCreateRejectsEmptyRecipient(t *testing.T) {
	svc := NewService(nil)
	if _, err := svc.Create(context.Background(), "Family", ChannelTelegram, "  "); err == nil {
		t.Fatal("expected error for empty recipient")
	}
}

func TestCreateRejectsUnknownChannel(t *testing.T) {
	svc := NewService(nil)
	if _, err := svc.Create(context.Background(), "Family", "signal", "123"); err == nil {
		t.Fatal("expected error for unknown channel")
	}
}
