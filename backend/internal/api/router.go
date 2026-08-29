package api

import (
	"log/slog"
	"net/http"

	"github.com/go-chi/chi/v5"
	chimw "github.com/go-chi/chi/v5/middleware"

	"tinyvoice/backend/internal/api/middleware"
	"tinyvoice/backend/internal/device"
	"tinyvoice/backend/internal/message"
	"tinyvoice/backend/internal/storage"
)

type Deps struct {
	Devices         *device.Service
	DeviceRepo      *device.Repository
	Messages        *message.Service
	Storage         storage.Provider
	EvolutionClient evolutionMediaClient
	Logger          *slog.Logger
	WebhookSecret   string
}

func NewRouter(deps Deps) http.Handler {
	r := chi.NewRouter()
	r.Use(chimw.Recoverer)
	r.Use(chimw.RealIP)
	r.Use(middleware.RequestLogger(deps.Logger))

	r.Get("/health", Health)

	deviceH := NewDeviceHandlers(deps.Devices, deps.Logger)
	msgH := NewMessageHandlers(deps.Messages, deps.DeviceRepo, deps.Storage, deps.Logger)
	webhookH := NewWebhookHandlers(deps.Messages, deps.DeviceRepo, deps.Storage, deps.EvolutionClient, deps.WebhookSecret, deps.Logger)

	r.Route("/api/v1", func(r chi.Router) {
		r.Post("/webhooks/evolution", webhookH.Evolution)

		r.Group(func(r chi.Router) {
			r.Use(middleware.DeviceAuth(deps.Devices))
			r.Post("/device/heartbeat", deviceH.Heartbeat)
			r.Get("/device/messages/next", msgH.Next)
			r.Post("/device/messages", msgH.Upload)
			r.Get("/device/messages/{id}/audio", msgH.DownloadAudio)
			r.Post("/device/messages/{id}/played", msgH.Played)
		})
	})

	return r
}
