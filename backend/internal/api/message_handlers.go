package api

import (
	"encoding/json"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"os"
	"strings"

	"github.com/go-chi/chi/v5"
	"github.com/google/uuid"

	"tinyvoice/backend/internal/api/middleware"
	"tinyvoice/backend/internal/audio"
	"tinyvoice/backend/internal/device"
	"tinyvoice/backend/internal/message"
	"tinyvoice/backend/internal/storage"
)

type MessageHandlers struct {
	messages   *message.Service
	devices    *device.Repository
	storage    storage.Provider
	logger     *slog.Logger
	maxUpload  int64
}

func NewMessageHandlers(
	messages *message.Service,
	devices *device.Repository,
	storage storage.Provider,
	logger *slog.Logger,
) *MessageHandlers {
	return &MessageHandlers{
		messages:  messages,
		devices:   devices,
		storage:   storage,
		logger:    logger,
		maxUpload: 5 * 1024 * 1024,
	}
}

func (h *MessageHandlers) Upload(w http.ResponseWriter, r *http.Request) {
	d := middleware.DeviceFromContext(r.Context())
	if d == nil {
		http.Error(w, `{"error":"unauthorized"}`, http.StatusUnauthorized)
		return
	}

	if err := r.ParseMultipartForm(h.maxUpload); err != nil {
		http.Error(w, `{"error":"invalid multipart"}`, http.StatusBadRequest)
		return
	}

	file, header, err := r.FormFile("audio")
	if err != nil {
		http.Error(w, `{"error":"missing audio field"}`, http.StatusBadRequest)
		return
	}
	defer file.Close()

	if header.Size > h.maxUpload {
		http.Error(w, `{"error":"file too large"}`, http.StatusBadRequest)
		return
	}

	tmpPath, err := audio.SaveTemp("upload", file)
	if err != nil {
		http.Error(w, `{"error":"save failed"}`, http.StatusInternalServerError)
		return
	}
	defer os.Remove(tmpPath)

	f, err := os.Open(tmpPath)
	if err != nil {
		http.Error(w, `{"error":"read failed"}`, http.StatusInternalServerError)
		return
	}
	defer f.Close()

	info, err := audio.ValidateWAV(f, h.maxUpload)
	if err != nil {
		http.Error(w, fmt.Sprintf(`{"error":%q}`, err.Error()), http.StatusBadRequest)
		return
	}

	if _, err := f.Seek(0, io.SeekStart); err != nil {
		http.Error(w, `{"error":"read failed"}`, http.StatusInternalServerError)
		return
	}

	msg, err := h.messages.CreateOutbound(r.Context(), d.ID, f, info.SizeBytes, "audio/wav", info.DurationMs)
	if err != nil {
		h.logger.Error("recording_upload_failed", slog.String("device_id", d.ID), slog.String("error", err.Error()))
		http.Error(w, `{"error":"upload failed"}`, http.StatusInternalServerError)
		return
	}

	h.logger.Info("recording_uploaded", slog.String("device_id", d.ID), slog.String("message_id", msg.ID))
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusCreated)
	_ = json.NewEncoder(w).Encode(map[string]string{
		"id":     msg.ID,
		"status": strings.ToLower(msg.Status),
	})
}

func (h *MessageHandlers) Next(w http.ResponseWriter, r *http.Request) {
	d := middleware.DeviceFromContext(r.Context())
	if d == nil {
		http.Error(w, `{"error":"unauthorized"}`, http.StatusUnauthorized)
		return
	}

	msg, err := h.messages.GetNextAvailable(r.Context(), d.ID)
	if err != nil {
		http.Error(w, `{"error":"internal error"}`, http.StatusInternalServerError)
		return
	}

	w.Header().Set("Content-Type", "application/json")
	if msg == nil {
		_ = json.NewEncoder(w).Encode(map[string]bool{"available": false})
		return
	}

	resp := map[string]interface{}{
		"available":   true,
		"id":          msg.ID,
		"duration_ms": msg.AudioDurationMs,
		"size_bytes":  msg.AudioSizeBytes,
	}
	_ = json.NewEncoder(w).Encode(resp)
}

func (h *MessageHandlers) DownloadAudio(w http.ResponseWriter, r *http.Request) {
	d := middleware.DeviceFromContext(r.Context())
	if d == nil {
		http.Error(w, `{"error":"unauthorized"}`, http.StatusUnauthorized)
		return
	}

	id := chi.URLParam(r, "id")
	if _, err := uuid.Parse(id); err != nil {
		http.Error(w, `{"error":"invalid id"}`, http.StatusBadRequest)
		return
	}

	rc, mime, size, err := h.messages.OpenAudio(r.Context(), id, d.ID)
	if err != nil {
		http.Error(w, `{"error":"not found"}`, http.StatusNotFound)
		return
	}
	defer rc.Close()

	w.Header().Set("Content-Type", mime)
	w.Header().Set("Content-Length", fmt.Sprintf("%d", size))
	if _, err := io.Copy(w, rc); err != nil {
		h.logger.Error("storage_error", slog.String("message_id", id))
	}
}

func (h *MessageHandlers) Played(w http.ResponseWriter, r *http.Request) {
	d := middleware.DeviceFromContext(r.Context())
	if d == nil {
		http.Error(w, `{"error":"unauthorized"}`, http.StatusUnauthorized)
		return
	}

	id := chi.URLParam(r, "id")
	if err := h.messages.MarkPlayed(r.Context(), id, d.ID); err != nil {
		http.Error(w, `{"error":"not found"}`, http.StatusNotFound)
		return
	}

	h.logger.Info("message_played", slog.String("device_id", d.ID), slog.String("message_id", id))
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(map[string]string{"status": "played"})
}
