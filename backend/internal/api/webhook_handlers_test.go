package api

import (
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

const (
	testLIDJID    = "123456789012345@lid"
	testPhoneJID  = "5511000000001@s.whatsapp.net"
	testPhoneE164 = "5511000000001"
)

func TestResolveRecipientUsesRemoteJidAltForLID(t *testing.T) {
	got := resolveRecipient(testLIDJID, testPhoneJID, "")
	if got != testPhoneE164 {
		t.Fatalf("got %q, want %s", got, testPhoneE164)
	}
}

func TestResolveRecipientUsesSenderPnForLID(t *testing.T) {
	got := resolveRecipient(testLIDJID, "", testPhoneJID)
	if got != testPhoneE164 {
		t.Fatalf("got %q, want %s", got, testPhoneE164)
	}
}

func TestResolveRecipientUsesRemoteJidWhenNotLID(t *testing.T) {
	got := resolveRecipient(testPhoneJID, "", "")
	if got != testPhoneE164 {
		t.Fatalf("got %q, want %s", got, testPhoneE164)
	}
}

func TestTelegramAudioPrefersVoice(t *testing.T) {
	update := telegramUpdate{
		Message: &telegramMessage{
			Voice: &telegramVoice{FileID: "voice-1", Duration: 3, MimeType: "audio/ogg"},
			Audio: &telegramVoice{FileID: "audio-1", Duration: 8, MimeType: "audio/mpeg"},
		},
	}
	fileID, mime, dur := telegramAudio(update)
	if fileID != "voice-1" || mime != "audio/ogg" || dur != 3 {
		t.Fatalf("got file=%s mime=%s dur=%d", fileID, mime, dur)
	}
}

func TestTelegramAudioFallsBackToAudio(t *testing.T) {
	update := telegramUpdate{
		Message: &telegramMessage{
			Audio: &telegramVoice{FileID: "audio-1", Duration: 8, MimeType: "audio/mpeg"},
		},
	}
	fileID, mime, dur := telegramAudio(update)
	if fileID != "audio-1" || mime != "audio/mpeg" || dur != 8 {
		t.Fatalf("got file=%s mime=%s dur=%d", fileID, mime, dur)
	}
}

func TestTelegramAudioIgnoresTextOnly(t *testing.T) {
	update := telegramUpdate{Message: &telegramMessage{}}
	fileID, _, _ := telegramAudio(update)
	if fileID != "" {
		t.Fatalf("expected empty file id, got %q", fileID)
	}
}

func TestTelegramWebhookRejectsBadSecret(t *testing.T) {
	h := &WebhookHandlers{telegramWebhookSecret: "expected-secret"}
	req := httptest.NewRequest(http.MethodPost, "/api/v1/webhooks/telegram", strings.NewReader(`{"update_id":1}`))
	req.Header.Set("X-Telegram-Bot-Api-Secret-Token", "wrong")
	rec := httptest.NewRecorder()
	h.Telegram(rec, req)
	if rec.Code != http.StatusUnauthorized {
		t.Fatalf("status=%d", rec.Code)
	}
}

func TestTelegramWebhookIgnoresNonAudio(t *testing.T) {
	h := &WebhookHandlers{telegramWebhookSecret: "secret"}
	req := httptest.NewRequest(http.MethodPost, "/api/v1/webhooks/telegram", strings.NewReader(`{
		"update_id": 1,
		"message": {"message_id": 2, "chat": {"id": 99}, "text": "hi"}
	}`))
	req.Header.Set("X-Telegram-Bot-Api-Secret-Token", "secret")
	rec := httptest.NewRecorder()
	h.Telegram(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("status=%d body=%s", rec.Code, rec.Body.String())
	}
}
