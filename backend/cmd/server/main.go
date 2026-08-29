package main

import (
	"context"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"syscall"
	"time"

	"tinyvoice/backend/internal/api"
	"tinyvoice/backend/internal/config"
	"tinyvoice/backend/internal/conversation"
	"tinyvoice/backend/internal/database"
	"tinyvoice/backend/internal/device"
	"tinyvoice/backend/internal/message"
	"tinyvoice/backend/internal/messaging/evolution"
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

	router := api.NewRouter(api.Deps{
		Devices:       deviceSvc,
		DeviceRepo:    deviceRepo,
		Messages:      msgSvc,
		Storage:       store,
		Logger:        logger,
		WebhookSecret: cfg.EvolutionWebhookSecret,
	})

	if cfg.EvolutionAPIKey != "" {
		provider := evolution.NewProvider(cfg.EvolutionBaseURL, cfg.EvolutionAPIKey, cfg.EvolutionInstance)
		w := worker.NewOutboundWorker(msgSvc, store, provider, cfg.WorkerMaxAttempts, cfg.WorkerPollInterval, logger)
		go w.Run(ctx)
		logger.Info("worker_started")
	}

	srv := &http.Server{
		Addr:         ":" + cfg.APIPort,
		Handler:      router,
		ReadTimeout:  30 * time.Second,
		WriteTimeout: 120 * time.Second,
		IdleTimeout:  60 * time.Second,
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
