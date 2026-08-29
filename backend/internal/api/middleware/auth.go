package middleware

import (
	"context"
	"log/slog"
	"net/http"
	"strings"
	"time"

	"tinyvoice/backend/internal/device"
)

type contextKey string

const DeviceContextKey contextKey = "device"

func RequestLogger(logger *slog.Logger) func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			start := time.Now()
			next.ServeHTTP(w, r)
			logger.Info("http_request",
				slog.String("method", r.Method),
				slog.String("path", r.URL.Path),
				slog.Duration("duration", time.Since(start)),
			)
		})
	}
}

func DeviceAuth(devices *device.Service) func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			auth := r.Header.Get("Authorization")
			if !strings.HasPrefix(auth, "Bearer ") {
				http.Error(w, `{"error":"unauthorized"}`, http.StatusUnauthorized)
				return
			}
			token := strings.TrimPrefix(auth, "Bearer ")
			d, err := devices.Authenticate(r.Context(), token)
			if err != nil {
				http.Error(w, `{"error":"unauthorized"}`, http.StatusUnauthorized)
				return
			}
			ctx := context.WithValue(r.Context(), DeviceContextKey, d)
			next.ServeHTTP(w, r.WithContext(ctx))
		})
	}
}

func DeviceFromContext(ctx context.Context) *device.Device {
	d, _ := ctx.Value(DeviceContextKey).(*device.Device)
	return d
}
