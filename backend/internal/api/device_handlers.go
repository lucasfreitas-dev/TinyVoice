package api

import (
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"strconv"

	"tinyvoice/backend/internal/api/middleware"
	"tinyvoice/backend/internal/device"
	"tinyvoice/backend/internal/firmware"
)

const deviceJSONLimit = 64 << 10

type DeviceHandlers struct {
	devices  *device.Service
	firmware *firmware.Service
	logger   *slog.Logger
}

func NewDeviceHandlers(devices *device.Service, fw *firmware.Service, logger *slog.Logger) *DeviceHandlers {
	return &DeviceHandlers{devices: devices, firmware: fw, logger: logger}
}

type heartbeatRequest struct {
	FirmwareVersion string         `json:"firmware_version"`
	UptimeMs        *int64         `json:"uptime_ms"`
	FreeHeap        *int64         `json:"free_heap"`
	RSSI            *int           `json:"rssi"`
	Logs            []device.LogIn `json:"logs"`
}

type firmwareOffer struct {
	UpdateAvailable bool   `json:"update_available"`
	Version         string `json:"version,omitempty"`
	SizeBytes       int64  `json:"size_bytes,omitempty"`
	SHA256          string `json:"sha256,omitempty"`
}

type heartbeatResponse struct {
	Status   string        `json:"status"`
	Firmware firmwareOffer `json:"firmware"`
	Logs     *logsAccepted `json:"logs,omitempty"`
}

type logsAccepted struct {
	Accepted int `json:"accepted"`
}

type logsRequest struct {
	FirmwareVersion string         `json:"firmware_version"`
	UptimeMs        *int64         `json:"uptime_ms"`
	Entries         []device.LogIn `json:"entries"`
}

func (h *DeviceHandlers) Heartbeat(w http.ResponseWriter, r *http.Request) {
	d := middleware.DeviceFromContext(r.Context())
	if d == nil {
		http.Error(w, `{"error":"unauthorized"}`, http.StatusUnauthorized)
		return
	}

	req, err := decodeHeartbeat(r)
	if err != nil {
		http.Error(w, `{"error":"invalid json"}`, http.StatusBadRequest)
		return
	}

	runtime := device.RuntimeUpdate{
		FirmwareVersion: req.FirmwareVersion,
		RSSI:            req.RSSI,
		FreeHeap:        req.FreeHeap,
		UptimeMs:        req.UptimeMs,
	}
	if err := h.devices.RecordHeartbeat(r.Context(), d.ID, runtime); err != nil {
		http.Error(w, `{"error":"internal error"}`, http.StatusInternalServerError)
		return
	}

	resp := heartbeatResponse{Status: "ok"}
	if len(req.Logs) > 0 {
		n, err := h.devices.InsertLogs(r.Context(), d.ID, req.FirmwareVersion, req.Logs)
		if err != nil {
			writeJSON(w, http.StatusBadRequest, map[string]string{"error": err.Error()})
			return
		}
		resp.Logs = &logsAccepted{Accepted: n}
	}

	offer, err := h.firmwareOffer(r, d.ID, req.FirmwareVersion)
	if err != nil {
		http.Error(w, `{"error":"internal error"}`, http.StatusInternalServerError)
		return
	}
	resp.Firmware = offer

	h.logger.Info("device_connected",
		slog.String("device_id", d.ID),
		slog.String("firmware_version", req.FirmwareVersion),
		slog.Bool("update_available", offer.UpdateAvailable),
	)
	writeJSON(w, http.StatusOK, resp)
}

func (h *DeviceHandlers) Logs(w http.ResponseWriter, r *http.Request) {
	d := middleware.DeviceFromContext(r.Context())
	if d == nil {
		http.Error(w, `{"error":"unauthorized"}`, http.StatusUnauthorized)
		return
	}

	r.Body = http.MaxBytesReader(w, r.Body, deviceJSONLimit)
	var req logsRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, `{"error":"invalid json"}`, http.StatusBadRequest)
		return
	}

	n, err := h.devices.InsertLogs(r.Context(), d.ID, req.FirmwareVersion, req.Entries)
	if err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": err.Error()})
		return
	}

	writeJSON(w, http.StatusOK, map[string]any{"status": "ok", "accepted": n})
}

func (h *DeviceHandlers) FirmwareBinary(w http.ResponseWriter, r *http.Request) {
	d := middleware.DeviceFromContext(r.Context())
	if d == nil {
		http.Error(w, `{"error":"unauthorized"}`, http.StatusUnauthorized)
		return
	}

	rel, reader, err := h.firmware.OpenBinary(r.Context(), d.ID)
	if err != nil {
		http.Error(w, `{"error":"firmware unavailable"}`, http.StatusInternalServerError)
		return
	}
	if rel == nil || reader == nil {
		http.Error(w, `{"error":"no firmware assigned"}`, http.StatusNotFound)
		return
	}
	defer reader.Close()

	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Content-Length", strconv.FormatInt(rel.SizeBytes, 10))
	w.Header().Set("X-Firmware-Version", rel.Version)
	w.Header().Set("X-Firmware-SHA256", rel.SHA256)
	w.WriteHeader(http.StatusOK)
	if _, err := io.Copy(w, reader); err != nil {
		h.logger.Error("firmware_stream_failed", slog.String("device_id", d.ID), slog.String("error", err.Error()))
	}
}

func (h *DeviceHandlers) firmwareOffer(r *http.Request, deviceID, current string) (firmwareOffer, error) {
	if h.firmware == nil {
		return firmwareOffer{}, nil
	}
	rel, err := h.firmware.DesiredForDevice(r.Context(), deviceID)
	if err != nil {
		return firmwareOffer{}, err
	}
	if rel == nil || !firmware.ShouldUpdate(current, rel.Version) {
		return firmwareOffer{}, nil
	}
	return firmwareOffer{
		UpdateAvailable: true,
		Version:         rel.Version,
		SizeBytes:       rel.SizeBytes,
		SHA256:          rel.SHA256,
	}, nil
}

func decodeHeartbeat(r *http.Request) (heartbeatRequest, error) {
	var req heartbeatRequest
	if r.Body == nil {
		return req, nil
	}
	r.Body = http.MaxBytesReader(nil, r.Body, deviceJSONLimit)
	dec := json.NewDecoder(r.Body)
	if err := dec.Decode(&req); err != nil {
		if err == io.EOF {
			return heartbeatRequest{}, nil
		}
		return heartbeatRequest{}, err
	}
	return req, nil
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}
