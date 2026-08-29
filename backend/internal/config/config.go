package config

import (
	"fmt"
	"os"
	"strconv"
	"time"
)

type Config struct {
	DatabaseURL           string
	APIPort               string
	AdminToken            string
	MinIOEndpoint         string
	MinIOAccessKey        string
	MinIOSecretKey        string
	MinIOBucket           string
	MinIOUseSSL           bool
	EvolutionBaseURL      string
	EvolutionAPIKey       string
	EvolutionInstance     string
	EvolutionWebhookSecret string
	PublicURL             string
	WorkerPollInterval    time.Duration
	WorkerMaxAttempts     int
}

func Load() (*Config, error) {
	pollInterval := 5 * time.Second
	if v := os.Getenv("WORKER_POLL_INTERVAL"); v != "" {
		d, err := time.ParseDuration(v)
		if err != nil {
			return nil, fmt.Errorf("invalid WORKER_POLL_INTERVAL: %w", err)
		}
		pollInterval = d
	}

	maxAttempts := 5
	if v := os.Getenv("WORKER_MAX_ATTEMPTS"); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil {
			return nil, fmt.Errorf("invalid WORKER_MAX_ATTEMPTS: %w", err)
		}
		maxAttempts = n
	}

	useSSL := os.Getenv("MINIO_USE_SSL") == "true"

	cfg := &Config{
		DatabaseURL:            envOr("DATABASE_URL", "postgres://tinyvoice:tinyvoice@localhost:5432/tinyvoice?sslmode=disable"),
		APIPort:                envOr("API_PORT", "8080"),
		AdminToken:             os.Getenv("ADMIN_TOKEN"),
		MinIOEndpoint:          envOr("MINIO_ENDPOINT", "localhost:9000"),
		MinIOAccessKey:         envOr("MINIO_ACCESS_KEY", "tinyvoice"),
		MinIOSecretKey:         envOr("MINIO_SECRET_KEY", "tinyvoice"),
		MinIOBucket:            envOr("MINIO_BUCKET", "tinyvoice-audio"),
		MinIOUseSSL:            useSSL,
		EvolutionBaseURL:       envOr("EVOLUTION_BASE_URL", "http://localhost:8081"),
		EvolutionAPIKey:        os.Getenv("EVOLUTION_API_KEY"),
		EvolutionInstance:      envOr("EVOLUTION_INSTANCE", "tinyvoice"),
		EvolutionWebhookSecret: os.Getenv("EVOLUTION_WEBHOOK_SECRET"),
		PublicURL:              envOr("TINYVOICE_PUBLIC_URL", "http://localhost:8080"),
		WorkerPollInterval:     pollInterval,
		WorkerMaxAttempts:      maxAttempts,
	}

	return cfg, nil
}

func envOr(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}
