package telegram

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"tinyvoice/backend/internal/messaging"
)

func TestSendVoicePostsMultipart(t *testing.T) {
	var gotChatID, gotDuration, gotFileName string
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/bottest-token/sendVoice" {
			t.Fatalf("unexpected path %s", r.URL.Path)
		}
		if err := r.ParseMultipartForm(1 << 20); err != nil {
			t.Fatalf("parse form: %v", err)
		}
		gotChatID = r.FormValue("chat_id")
		gotDuration = r.FormValue("duration")
		_, hdr, err := r.FormFile("voice")
		if err != nil {
			t.Fatalf("voice file: %v", err)
		}
		gotFileName = hdr.Filename
		_ = json.NewEncoder(w).Encode(map[string]any{"ok": true, "result": map[string]any{}})
	}))
	defer srv.Close()

	p := NewProviderWithClient(NewClientWithBase("test-token", srv.URL))
	err := p.SendAudio(context.Background(), "123456789", messaging.AudioMessage{
		Reader:      io.NopCloser(strings.NewReader("opus-bytes")),
		Size:        10,
		MimeType:    "audio/ogg",
		DurationSec: 4,
		FileName:    "voice.ogg",
	})
	if err != nil {
		t.Fatal(err)
	}
	if gotChatID != "123456789" {
		t.Fatalf("chat_id=%q", gotChatID)
	}
	if gotDuration != "4" {
		t.Fatalf("duration=%q", gotDuration)
	}
	if gotFileName != "voice.ogg" {
		t.Fatalf("filename=%q", gotFileName)
	}
}

func TestDownloadFileUsesGetFilePath(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch {
		case strings.HasSuffix(r.URL.Path, "/getFile"):
			_ = json.NewEncoder(w).Encode(map[string]any{
				"ok": true,
				"result": map[string]any{
					"file_id":   "file-1",
					"file_path": "voice/file-1.oga",
				},
			})
		case strings.Contains(r.URL.Path, "/file/bot"):
			w.Header().Set("Content-Type", "audio/ogg")
			_, _ = w.Write([]byte("oga-bytes"))
		default:
			t.Fatalf("unexpected path %s", r.URL.Path)
		}
	}))
	defer srv.Close()

	p := NewProviderWithClient(NewClientWithBase("test-token", srv.URL))
	rc, size, mime, err := p.DownloadMedia(context.Background(), "file-1")
	if err != nil {
		t.Fatal(err)
	}
	defer rc.Close()
	data, err := io.ReadAll(rc)
	if err != nil {
		t.Fatal(err)
	}
	if size != 9 || !bytes.Equal(data, []byte("oga-bytes")) {
		t.Fatalf("unexpected download size=%d data=%q", size, data)
	}
	if mime != "audio/ogg" {
		t.Fatalf("mime=%q", mime)
	}
}

func TestSetWebhookIncludesSecret(t *testing.T) {
	var payload map[string]any
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if !strings.HasSuffix(r.URL.Path, "/setWebhook") {
			t.Fatalf("unexpected path %s", r.URL.Path)
		}
		if err := json.NewDecoder(r.Body).Decode(&payload); err != nil {
			t.Fatal(err)
		}
		_ = json.NewEncoder(w).Encode(map[string]any{"ok": true, "result": true})
	}))
	defer srv.Close()

	c := NewClientWithBase("test-token", srv.URL)
	if err := c.SetWebhook(context.Background(), "https://tinyvoice.example.com/api/v1/webhooks/telegram", "secret"); err != nil {
		t.Fatal(err)
	}
	if payload["url"] != "https://tinyvoice.example.com/api/v1/webhooks/telegram" {
		t.Fatalf("url=%v", payload["url"])
	}
	if payload["secret_token"] != "secret" {
		t.Fatalf("secret_token=%v", payload["secret_token"])
	}
}
