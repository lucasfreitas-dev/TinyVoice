package api

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"os"
	"strings"

	"github.com/google/uuid"

	"tinyvoice/backend/internal/audio"
	"tinyvoice/backend/internal/device"
	"tinyvoice/backend/internal/message"
	"tinyvoice/backend/internal/storage"
)

type evolutionMediaClient interface {
	GetBase64FromMediaMessage(ctx context.Context, messageID, remoteJid string) ([]byte, string, error)
}

type WebhookHandlers struct {
	messages  *message.Service
	devices   *device.Repository
	storage   storage.Provider
	evolution evolutionMediaClient
	secret    string
	logger    *slog.Logger
}

func NewWebhookHandlers(
	messages *message.Service,
	devices *device.Repository,
	storage storage.Provider,
	evolutionClient evolutionMediaClient,
	secret string,
	logger *slog.Logger,
) *WebhookHandlers {
	return &WebhookHandlers{
		messages:  messages,
		devices:   devices,
		storage:   storage,
		evolution: evolutionClient,
		secret:    secret,
		logger:    logger,
	}
}

type evolutionWebhook struct {
	Event    string `json:"event"`
	Instance string `json:"instance"`
	Data     struct {
		Key struct {
			ID           string `json:"id"`
			RemoteJid    string `json:"remoteJid"`
			RemoteJidAlt string `json:"remoteJidAlt"`
			SenderPn     string `json:"senderPn"`
			FromMe       bool   `json:"fromMe"`
		} `json:"key"`
		Message struct {
			AudioMessage *struct {
				URL      string `json:"url"`
				Mimetype string `json:"mimetype"`
				Seconds  int    `json:"seconds"`
				Ptt      bool   `json:"ptt"`
			} `json:"audioMessage"`
		} `json:"message"`
		MessageType string `json:"messageType"`
	} `json:"data"`
}

func (h *WebhookHandlers) Evolution(w http.ResponseWriter, r *http.Request) {
	if h.secret != "" && r.Header.Get("apikey") != h.secret {
		http.Error(w, `{"error":"unauthorized"}`, http.StatusUnauthorized)
		return
	}

	body, err := io.ReadAll(r.Body)
	if err != nil {
		http.Error(w, `{"error":"read body"}`, http.StatusBadRequest)
		return
	}

	var payload evolutionWebhook
	if err := json.Unmarshal(body, &payload); err != nil {
		http.Error(w, `{"error":"invalid json"}`, http.StatusBadRequest)
		return
	}

	if payload.Event != "messages.upsert" && payload.Event != "MESSAGES_UPSERT" {
		w.WriteHeader(http.StatusOK)
		return
	}

	if payload.Data.Key.FromMe {
		w.WriteHeader(http.StatusOK)
		return
	}

	if payload.Data.Message.AudioMessage == nil && payload.Data.MessageType != "audioMessage" {
		w.WriteHeader(http.StatusOK)
		return
	}

	externalID := payload.Data.Key.ID
	if externalID == "" {
		http.Error(w, `{"error":"missing message id"}`, http.StatusBadRequest)
		return
	}

	newEvent, err := h.messages.RecordWebhookEvent(r.Context(), "evolution", externalID)
	if err != nil {
		http.Error(w, `{"error":"internal error"}`, http.StatusInternalServerError)
		return
	}
	if !newEvent {
		w.WriteHeader(http.StatusOK)
		_ = json.NewEncoder(w).Encode(map[string]string{"status": "duplicate"})
		return
	}

	recipient := resolveRecipient(payload.Data.Key.RemoteJid, payload.Data.Key.RemoteJidAlt, payload.Data.Key.SenderPn)
	deviceID, convID, err := h.devices.FindByRecipient(r.Context(), recipient)
	if err != nil {
		h.logger.Error("webhook_no_device",
			slog.String("recipient", recipient),
			slog.String("remote_jid", payload.Data.Key.RemoteJid),
			slog.String("remote_jid_alt", payload.Data.Key.RemoteJidAlt),
		)
		w.WriteHeader(http.StatusOK)
		return
	}

	audioData, mime, err := h.downloadInboundAudio(r.Context(), payload)
	if err != nil {
		h.logger.Error("whatsapp_error", slog.String("error", err.Error()))
		http.Error(w, `{"error":"download failed"}`, http.StatusInternalServerError)
		return
	}
	defer func() {
		if c, ok := audioData.(io.Closer); ok {
			c.Close()
		}
	}()

	tmpIn, err := os.CreateTemp("", "inbound-*"+inboundExt(mime))
	if err != nil {
		http.Error(w, `{"error":"temp file"}`, http.StatusInternalServerError)
		return
	}
	inPath := tmpIn.Name()
	defer os.Remove(inPath)
	defer tmpIn.Close()

	if _, err := io.Copy(tmpIn, audioData); err != nil {
		http.Error(w, `{"error":"save failed"}`, http.StatusInternalServerError)
		return
	}
	tmpIn.Close()

	wavPath := inPath + ".wav"
	defer os.Remove(wavPath)
	if err := audio.ConvertToWAV(inPath, wavPath); err != nil {
		h.logger.Error("whatsapp_error", slog.String("error", err.Error()))
		http.Error(w, `{"error":"convert failed"}`, http.StatusInternalServerError)
		return
	}
	if am := payload.Data.Message.AudioMessage; am != nil && am.Seconds > audio.MaxDurationMs/1000 {
		h.logger.Info("inbound_audio_trimmed",
			slog.String("external_id", externalID),
			slog.Int("original_seconds", am.Seconds),
			slog.Int("kept_seconds", audio.MaxDurationMs/1000),
		)
	}

	wavFile, err := os.Open(wavPath)
	if err != nil {
		http.Error(w, `{"error":"open wav"}`, http.StatusInternalServerError)
		return
	}
	defer wavFile.Close()

	wavInfo, err := wavFile.Stat()
	if err != nil {
		http.Error(w, `{"error":"stat wav"}`, http.StatusInternalServerError)
		return
	}

	if _, err := wavFile.Seek(0, io.SeekStart); err != nil {
		http.Error(w, `{"error":"seek wav"}`, http.StatusInternalServerError)
		return
	}

	validInfo, err := audio.ValidateWAV(wavFile, 10*1024*1024)
	if err != nil {
		http.Error(w, `{"error":"invalid audio"}`, http.StatusBadRequest)
		return
	}

	if _, err := wavFile.Seek(0, io.SeekStart); err != nil {
		http.Error(w, `{"error":"seek wav"}`, http.StatusInternalServerError)
		return
	}

	key := fmt.Sprintf("audio/inbound/%s", uuid.New().String())
	if err := h.storage.Put(r.Context(), key, wavFile, wavInfo.Size(), "audio/wav"); err != nil {
		h.logger.Error("storage_error", slog.String("error", err.Error()))
		http.Error(w, `{"error":"storage failed"}`, http.StatusInternalServerError)
		return
	}

	_, err = h.messages.CreateInbound(r.Context(), deviceID, convID, externalID, key, "audio/wav", validInfo.DurationMs, wavInfo.Size())
	if err != nil {
		if strings.Contains(err.Error(), "duplicate") {
			w.WriteHeader(http.StatusOK)
			return
		}
		http.Error(w, `{"error":"create message"}`, http.StatusInternalServerError)
		return
	}

	h.logger.Info("message_received", slog.String("device_id", deviceID), slog.String("external_id", externalID))
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(map[string]string{"status": "ok"})
}

func normalizePhone(remoteJid string) string {
	p := strings.Split(remoteJid, "@")[0]
	p = strings.TrimPrefix(p, "+")
	return p
}

func resolveRecipient(remoteJid, remoteJidAlt, senderPn string) string {
	if strings.Contains(remoteJid, "@lid") {
		if remoteJidAlt != "" {
			return normalizePhone(remoteJidAlt)
		}
		if senderPn != "" {
			return normalizePhone(senderPn)
		}
	}
	return normalizePhone(remoteJid)
}

func (h *WebhookHandlers) downloadInboundAudio(ctx context.Context, payload evolutionWebhook) (io.Reader, string, error) {
	if h.evolution != nil && payload.Data.Key.ID != "" && payload.Data.Key.RemoteJid != "" {
		data, mime, err := h.evolution.GetBase64FromMediaMessage(
			ctx,
			payload.Data.Key.ID,
			payload.Data.Key.RemoteJid,
		)
		if err != nil {
			h.logger.Warn("evolution_media_download_failed", slog.String("error", err.Error()))
		} else if len(data) > 0 {
			if mime == "" && payload.Data.Message.AudioMessage != nil {
				mime = payload.Data.Message.AudioMessage.Mimetype
			}
			return bytes.NewReader(data), mime, nil
		}
	}

	if payload.Data.Message.AudioMessage != nil && payload.Data.Message.AudioMessage.URL != "" {
		resp, err := http.Get(payload.Data.Message.AudioMessage.URL)
		if err != nil {
			return nil, "", err
		}
		if resp.StatusCode >= 300 {
			resp.Body.Close()
			return nil, "", fmt.Errorf("download status %d", resp.StatusCode)
		}
		mime := payload.Data.Message.AudioMessage.Mimetype
		return resp.Body, mime, nil
	}
	return nil, "", fmt.Errorf("no audio in payload")
}

func inboundExt(mime string) string {
	switch {
	case strings.Contains(mime, "ogg"):
		return ".ogg"
	case strings.Contains(mime, "mpeg"), strings.Contains(mime, "mp3"):
		return ".mp3"
	case strings.Contains(mime, "mp4"), strings.Contains(mime, "m4a"):
		return ".m4a"
	default:
		return ".bin"
	}
}
