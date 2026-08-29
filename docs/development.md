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

## Environment Variables

See [.env.example](../.env.example).

Never commit `.env` or `firmware/tinyvoice/include/config.h`.
