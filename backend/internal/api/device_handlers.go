package api

import (
	"encoding/json"
	"log/slog"
	"net/http"

	"tinyvoice/backend/internal/api/middleware"
	"tinyvoice/backend/internal/device"
)

type DeviceHandlers struct {
	devices *device.Service
	logger  *slog.Logger
}

func NewDeviceHandlers(devices *device.Service, logger *slog.Logger) *DeviceHandlers {
	return &DeviceHandlers{devices: devices, logger: logger}
}

func (h *DeviceHandlers) Heartbeat(w http.ResponseWriter, r *http.Request) {
	d := middleware.DeviceFromContext(r.Context())
	if d == nil {
		http.Error(w, `{"error":"unauthorized"}`, http.StatusUnauthorized)
		return
	}

	if err := h.devices.UpdateLastSeen(r.Context(), d.ID); err != nil {
		http.Error(w, `{"error":"internal error"}`, http.StatusInternalServerError)
		return
	}

	h.logger.Info("device_connected", slog.String("device_id", d.ID))
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(map[string]string{"status": "ok"})
}
