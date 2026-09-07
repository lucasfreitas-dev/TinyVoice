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
	"strconv"
	"strings"

	"github.com/google/uuid"

	"tinyvoice/backend/internal/audio"
	"tinyvoice/backend/internal/conversation"
	"tinyvoice/backend/internal/device"
	"tinyvoice/backend/internal/message"
	"tinyvoice/backend/internal/storage"
)

type evolutionMediaClient interface {
	GetBase64FromMediaMessage(ctx context.Context, messageID, remoteJid string) ([]byte, string, error)
}

type telegramMediaClient interface {
	DownloadFile(ctx context.Context, fileID string) ([]byte, string, error)
}

type WebhookHandlers struct {
	messages              *message.Service
	devices               *device.Repository
	storage               storage.Provider
	evolution             evolutionMediaClient
	telegram              telegramMediaClient
	evolutionSecret       string
	telegramWebhookSecret string
	logger                *slog.Logger
}

func NewWebhookHandlers(
	messages *message.Service,
	devices *device.Repository,
	storage storage.Provider,
	evolutionClient evolutionMediaClient,
	telegramClient telegramMediaClient,
	evolutionSecret string,
	telegramWebhookSecret string,
	logger *slog.Logger,
) *WebhookHandlers {
	return &WebhookHandlers{
		messages:              messages,
		devices:               devices,
		storage:               storage,
		evolution:             evolutionClient,
		telegram:              telegramClient,
		evolutionSecret:       evolutionSecret,
		telegramWebhookSecret: telegramWebhookSecret,
		logger:                logger,
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
	if h.evolutionSecret != "" && r.Header.Get("apikey") != h.evolutionSecret {
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
	deviceID, convID, err := h.devices.FindByRecipient(r.Context(), conversation.ChannelWhatsApp, recipient)
	if err != nil {
		h.logger.Error("webhook_no_device",
			slog.String("channel", conversation.ChannelWhatsApp),
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

	durationSec := 0
	if am := payload.Data.Message.AudioMessage; am != nil {
		durationSec = am.Seconds
	}

	if err := h.ingestInboundAudio(r.Context(), deviceID, convID, externalID, audioData, mime, durationSec); err != nil {
		h.writeIngestError(w, err)
		return
	}

	h.logger.Info("message_received",
		slog.String("channel", conversation.ChannelWhatsApp),
		slog.String("device_id", deviceID),
		slog.String("external_id", externalID),
	)
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(map[string]string{"status": "ok"})
}

type telegramUser struct {
	ID    int64 `json:"id"`
	IsBot bool  `json:"is_bot"`
}

type telegramChat struct {
	ID int64 `json:"id"`
}

type telegramVoice struct {
	FileID   string `json:"file_id"`
	Duration int    `json:"duration"`
	MimeType string `json:"mime_type"`
}

type telegramMessage struct {
	MessageID int64          `json:"message_id"`
	From      *telegramUser  `json:"from"`
	Chat      telegramChat   `json:"chat"`
	Voice     *telegramVoice `json:"voice"`
	Audio     *telegramVoice `json:"audio"`
	VideoNote *telegramVoice `json:"video_note"`
}

type telegramUpdate struct {
	UpdateID int64            `json:"update_id"`
	Message  *telegramMessage `json:"message"`
}

func (h *WebhookHandlers) logTelegramIgnored(reason string, attrs ...slog.Attr) {
	if h.logger == nil {
		return
	}
	args := make([]any, 0, len(attrs)+1)
	args = append(args, slog.String("reason", reason))
	for _, a := range attrs {
		args = append(args, a)
	}
	h.logger.Info("telegram_ignored", args...)
}

func (h *WebhookHandlers) Telegram(w http.ResponseWriter, r *http.Request) {
	if h.telegramWebhookSecret != "" && r.Header.Get("X-Telegram-Bot-Api-Secret-Token") != h.telegramWebhookSecret {
		h.logTelegramIgnored("unauthorized")
		http.Error(w, `{"error":"unauthorized"}`, http.StatusUnauthorized)
		return
	}

	body, err := io.ReadAll(r.Body)
	if err != nil {
		http.Error(w, `{"error":"read body"}`, http.StatusBadRequest)
		return
	}

	var update telegramUpdate
	if err := json.Unmarshal(body, &update); err != nil {
		http.Error(w, `{"error":"invalid json"}`, http.StatusBadRequest)
		return
	}

	if update.Message == nil {
		h.logTelegramIgnored("no_message", slog.Int64("update_id", update.UpdateID))
		w.WriteHeader(http.StatusOK)
		return
	}

	fileID, mime, durationSec := telegramAudio(update)
	if fileID == "" {
		h.logTelegramIgnored("no_audio",
			slog.Int64("chat_id", update.Message.Chat.ID),
			slog.Int64("message_id", update.Message.MessageID),
		)
		w.WriteHeader(http.StatusOK)
		return
	}

	recipient := strconv.FormatInt(update.Message.Chat.ID, 10)
	externalID := fmt.Sprintf("%d:%d", update.Message.Chat.ID, update.Message.MessageID)

	newEvent, err := h.messages.RecordWebhookEvent(r.Context(), "telegram", externalID)
	if err != nil {
		http.Error(w, `{"error":"internal error"}`, http.StatusInternalServerError)
		return
	}
	if !newEvent {
		w.WriteHeader(http.StatusOK)
		_ = json.NewEncoder(w).Encode(map[string]string{"status": "duplicate"})
		return
	}

	deviceID, convID, err := h.devices.FindByRecipient(r.Context(), conversation.ChannelTelegram, recipient)
	if err != nil {
		h.logger.Error("webhook_no_device",
			slog.String("channel", conversation.ChannelTelegram),
			slog.String("recipient", recipient),
		)
		w.WriteHeader(http.StatusOK)
		return
	}

	if h.telegram == nil {
		h.logger.Error("telegram_error", slog.String("error", "telegram client not configured"))
		http.Error(w, `{"error":"telegram not configured"}`, http.StatusInternalServerError)
		return
	}

	data, downloadedMime, err := h.telegram.DownloadFile(r.Context(), fileID)
	if err != nil {
		h.logger.Error("telegram_error", slog.String("error", err.Error()))
		http.Error(w, `{"error":"download failed"}`, http.StatusInternalServerError)
		return
	}
	if downloadedMime != "" {
		mime = downloadedMime
	}

	if err := h.ingestInboundAudio(r.Context(), deviceID, convID, externalID, bytes.NewReader(data), mime, durationSec); err != nil {
		h.writeIngestError(w, err)
		return
	}

	h.logger.Info("message_received",
		slog.String("channel", conversation.ChannelTelegram),
		slog.String("device_id", deviceID),
		slog.String("external_id", externalID),
	)
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(map[string]string{"status": "ok"})
}

func telegramAudio(update telegramUpdate) (fileID, mime string, durationSec int) {
	if update.Message == nil {
		return "", "", 0
	}
	if v := update.Message.Voice; v != nil && v.FileID != "" {
		return v.FileID, v.MimeType, v.Duration
	}
	if a := update.Message.Audio; a != nil && a.FileID != "" {
		return a.FileID, a.MimeType, a.Duration
	}
	if v := update.Message.VideoNote; v != nil && v.FileID != "" {
		mime := v.MimeType
		if mime == "" {
			mime = "video/mp4"
		}
		return v.FileID, mime, v.Duration
	}
	return "", "", 0
}

func (h *WebhookHandlers) ingestInboundAudio(
	ctx context.Context,
	deviceID, convID, externalID string,
	audioData io.Reader,
	mime string,
	durationSec int,
) error {
	tmpIn, err := os.CreateTemp("", "inbound-*"+inboundExt(mime))
	if err != nil {
		return fmt.Errorf("temp file: %w", err)
	}
	inPath := tmpIn.Name()
	defer os.Remove(inPath)
	defer tmpIn.Close()

	if _, err := io.Copy(tmpIn, audioData); err != nil {
		return fmt.Errorf("save failed: %w", err)
	}
	tmpIn.Close()

	wavPath := inPath + ".wav"
	defer os.Remove(wavPath)
	if err := audio.ConvertToWAV(inPath, wavPath); err != nil {
		return fmt.Errorf("convert failed: %w", err)
	}
	if durationSec > audio.MaxDurationMs/1000 {
		h.logger.Info("inbound_audio_trimmed",
			slog.String("external_id", externalID),
			slog.Int("original_seconds", durationSec),
			slog.Int("kept_seconds", audio.MaxDurationMs/1000),
		)
	}

	wavFile, err := os.Open(wavPath)
	if err != nil {
		return fmt.Errorf("open wav: %w", err)
	}
	defer wavFile.Close()

	wavInfo, err := wavFile.Stat()
	if err != nil {
		return fmt.Errorf("stat wav: %w", err)
	}

	if _, err := wavFile.Seek(0, io.SeekStart); err != nil {
		return fmt.Errorf("seek wav: %w", err)
	}

	validInfo, err := audio.ValidateWAV(wavFile, 10*1024*1024)
	if err != nil {
		return fmt.Errorf("invalid audio: %w", err)
	}

	if _, err := wavFile.Seek(0, io.SeekStart); err != nil {
		return fmt.Errorf("seek wav: %w", err)
	}

	key := fmt.Sprintf("audio/inbound/%s", uuid.New().String())
	if err := h.storage.Put(ctx, key, wavFile, wavInfo.Size(), "audio/wav"); err != nil {
		return fmt.Errorf("storage failed: %w", err)
	}

	_, err = h.messages.CreateInbound(ctx, deviceID, convID, externalID, key, "audio/wav", validInfo.DurationMs, wavInfo.Size())
	if err != nil {
		if strings.Contains(err.Error(), "duplicate") {
			return errDuplicateInbound
		}
		return fmt.Errorf("create message: %w", err)
	}
	return nil
}

var errDuplicateInbound = fmt.Errorf("duplicate inbound")

func (h *WebhookHandlers) writeIngestError(w http.ResponseWriter, err error) {
	switch {
	case err == nil:
		return
	case strings.Contains(err.Error(), "duplicate"):
		w.WriteHeader(http.StatusOK)
	case strings.Contains(err.Error(), "invalid audio"):
		h.logger.Error("inbound_invalid_audio", slog.String("error", err.Error()))
		http.Error(w, `{"error":"invalid audio"}`, http.StatusBadRequest)
	case strings.Contains(err.Error(), "convert failed"):
		h.logger.Error("inbound_convert_failed", slog.String("error", err.Error()))
		http.Error(w, `{"error":"convert failed"}`, http.StatusInternalServerError)
	case strings.Contains(err.Error(), "storage failed"):
		h.logger.Error("storage_error", slog.String("error", err.Error()))
		http.Error(w, `{"error":"storage failed"}`, http.StatusInternalServerError)
	default:
		h.logger.Error("inbound_ingest_failed", slog.String("error", err.Error()))
		http.Error(w, `{"error":"ingest failed"}`, http.StatusInternalServerError)
	}
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
	case strings.Contains(mime, "ogg"), strings.Contains(mime, "opus"):
		return ".ogg"
	case strings.Contains(mime, "mpeg"), strings.Contains(mime, "mp3"):
		return ".mp3"
	case strings.Contains(mime, "m4a"):
		return ".m4a"
	case strings.Contains(mime, "mp4"):
		return ".mp4"
	default:
		return ".bin"
	}
}
