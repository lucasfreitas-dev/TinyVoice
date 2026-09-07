package main

import "testing"

func TestTelegramWebhookURLRequiresHTTPS(t *testing.T) {
	if got := telegramWebhookURL("http://192.168.1.10"); got != "" {
		t.Fatalf("http LAN url should be skipped, got %q", got)
	}
	want := "https://tinyvoice.example.com/api/v1/webhooks/telegram"
	if got := telegramWebhookURL("https://tinyvoice.example.com/"); got != want {
		t.Fatalf("got %q, want %q", got, want)
	}
	if got := telegramWebhookURL("http://localhost:8080"); got == "" {
		t.Fatal("localhost should be allowed")
	}
}