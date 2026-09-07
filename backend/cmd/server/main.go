package main

import (
	"context"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"
	"time"

	"tinyvoice/backend/internal/api"
	"tinyvoice/backend/internal/config"
	"tinyvoice/backend/internal/conversation"
	"tinyvoice/backend/internal/database"
	"tinyvoice/backend/internal/device"
	"tinyvoice/backend/internal/message"
	"tinyvoice/backend/internal/messaging"
	"tinyvoice/backend/internal/messaging/evolution"
	"tinyvoice/backend/internal/messaging/telegram"
	"tinyvoice/backend/internal/storage"
	"tinyvoice/backend/internal/worker"
)

func main() {
	logger := slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{Level: slog.LevelInfo}))

	cfg, err := config.Load()
	if err != nil {
		logger.Error("config_error", slog.String("error", err.Error()))
		os.Exit(1)
	}

	ctx := context.Background()

	migrationsPath := os.Getenv("MIGRATIONS_PATH")
	if migrationsPath == "" {
		migrationsPath = filepath.Join("migrations")
	}
	if err := database.RunMigrations(cfg.DatabaseURL, migrationsPath); err != nil {
		logger.Error("migration_error", slog.String("error", err.Error()))
		os.Exit(1)
	}

	pool, err := database.NewPool(ctx, cfg.DatabaseURL)
	if err != nil {
		logger.Error("database_error", slog.String("error", err.Error()))
		os.Exit(1)
	}
	defer pool.Close()

	deviceRepo := device.NewRepository(pool)
	deviceSvc := device.NewService(deviceRepo)
	convRepo := conversation.NewRepository(pool)
	msgRepo := message.NewRepository(pool)

	store, err := storage.NewMinIO(cfg.MinIOEndpoint, cfg.MinIOAccessKey, cfg.MinIOSecretKey, cfg.MinIOBucket, cfg.MinIOUseSSL)
	if err != nil {
		logger.Error("storage_error", slog.String("error", err.Error()))
		os.Exit(1)
	}
	if err := store.EnsureBucket(ctx); err != nil {
		logger.Error("storage_error", slog.String("error", err.Error()))
		os.Exit(1)
	}

	msgSvc := message.NewService(msgRepo, deviceRepo, convRepo, store)

	var evolutionClient *evolution.Client
	if cfg.EvolutionAPIKey != "" {
		evolutionClient = evolution.NewClient(cfg.EvolutionBaseURL, cfg.EvolutionAPIKey, cfg.EvolutionInstance)
	}

	var telegramClient *telegram.Client
	if cfg.TelegramBotToken != "" {
		telegramClient = telegram.NewClient(cfg.TelegramBotToken)
	}

	router := api.NewRouter(api.Deps{
		Devices:               deviceSvc,
		DeviceRepo:            deviceRepo,
		Messages:              msgSvc,
		Storage:               store,
		EvolutionClient:       evolutionClient,
		TelegramClient:        telegramClient,
		Logger:                logger,
		WebhookSecret:         cfg.EvolutionWebhookSecret,
		TelegramWebhookSecret: cfg.TelegramWebhookSecret,
	})

	providers := map[string]messaging.MessagingProvider{}
	if cfg.EvolutionAPIKey != "" {
		providers[conversation.ChannelWhatsApp] = evolution.NewProvider(cfg.EvolutionBaseURL, cfg.EvolutionAPIKey, cfg.EvolutionInstance)
	}
	if telegramClient != nil {
		providers[conversation.ChannelTelegram] = telegram.NewProviderWithClient(telegramClient)
		if telegramWebhookURL(cfg.PublicURL) != "" {
			webhookURL := telegramWebhookURL(cfg.PublicURL)
			if err := telegramClient.SetWebhook(ctx, webhookURL, cfg.TelegramWebhookSecret); err != nil {
				logger.Error("telegram_webhook_register_failed", slog.String("error", err.Error()), slog.String("url", webhookURL))
			} else {
				logger.Info("telegram_webhook_registered", slog.String("url", webhookURL))
			}
		} else {
			logger.Info("telegram_webhook_skipped", slog.String("reason", "TINYVOICE_PUBLIC_URL must be https (or localhost) to register a Telegram webhook"))
		}
	}
	if len(providers) > 0 {
		w := worker.NewOutboundWorker(msgSvc, store, providers, cfg.WorkerMaxAttempts, cfg.WorkerPollInterval, logger)
		go w.Run(ctx)
		logger.Info("worker_started", slog.Int("channels", len(providers)))
	}

	srv := &http.Server{
		Addr:    ":" + cfg.APIPort,
		Handler: router,
		// ReadTimeout covers reading the *entire* request including the body, as one
		// absolute deadline. At 30s it silently cut off device uploads that took longer
		// than that to stream, which an ESP32 pushing a few hundred KB over Wi-Fi and TLS
		// routinely does. ReadHeaderTimeout still guards the header phase, and the upload
		// handler caps the payload size.
		ReadHeaderTimeout: 15 * time.Second,
		ReadTimeout:       300 * time.Second,
		WriteTimeout:      120 * time.Second,
		IdleTimeout:       60 * time.Second,
	}

	go func() {
		logger.Info("server_started", slog.String("port", cfg.APIPort))
		if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			logger.Error("server_error", slog.String("error", err.Error()))
			os.Exit(1)
		}
	}()

	quit := make(chan os.Signal, 1)
	signal.Notify(quit, syscall.SIGINT, syscall.SIGTERM)
	<-quit

	shutdownCtx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	_ = srv.Shutdown(shutdownCtx)
	logger.Info("server_stopped")
}

func telegramWebhookURL(publicURL string) string {
	base := strings.TrimRight(publicURL, "/")
	if base == "" {
		return ""
	}
	if !strings.HasPrefix(base, "https://") && !strings.Contains(base, "localhost") && !strings.Contains(base, "127.0.0.1") {
		return ""
	}
	return base + "/api/v1/webhooks/telegram"
}
