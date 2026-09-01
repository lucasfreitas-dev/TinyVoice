# Development

## Backend

### Requirements

- Go 1.22+
- PostgreSQL 16
- MinIO
- FFmpeg

### Local run

```bash
cp .env.example .env
# Start postgres and minio via docker compose
cd deploy && docker compose up -d postgres minio

cd backend
go mod tidy
export DATABASE_URL=postgres://tinyvoice:tinyvoice@localhost:5432/tinyvoice?sslmode=disable
go run ./cmd/server
```

### CLI

```bash
export ADMIN_TOKEN=your-admin-token
export DATABASE_URL=postgres://...

go run ./cmd/tinyvoice device create --name "Test Device"
go run ./cmd/tinyvoice device list
go run ./cmd/tinyvoice conversation create --name "Test" --recipient "5511000000001"
go run ./cmd/tinyvoice device bind --device <uuid> --conversation <uuid>
```

### Tests

```bash
cd backend
go test ./...
```

## Firmware

### Requirements

- [PlatformIO](https://platformio.org/)

### Setup

```bash
cp firmware/tinyvoice/include/config.example.h firmware/tinyvoice/include/config.h
# Edit credentials
cd firmware/tinyvoice
pio run
pio run -t upload
pio device monitor
```

### GPIO map

See [hardware.md](hardware.md).

### Volume pot

A 10 kΩ pot on **GPIO 34** (ADC1) scales playback. The floor (`VOLUME_MIN_GAIN`, default `0.18`) keeps the lowest setting audible. Boxes without a pot keep full volume.

Tune in `config.h` after copying `config.example.h`: `VOLUME_POT_ENABLED`, `VOLUME_MIN_GAIN`, `VOLUME_MAX_GAIN`.

## Environment Variables

See [.env.example](../.env.example).

Never commit `.env` or `firmware/tinyvoice/include/config.h`.
