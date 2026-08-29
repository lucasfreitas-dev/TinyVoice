# TinyVoice

TinyVoice is a physical audio messaging box based on ESP32. A child holds a button to record a voice message; the message is sent via Wi-Fi to a home-server backend and delivered to a parent through WhatsApp. When the parent replies with audio, an LED indicates a new message and the child presses the button to listen.

## Architecture

- **ESP32 firmware** — INMP441 microphone, MAX98357A amplifier, button, LED
- **Go API** — REST backend with PostgreSQL and MinIO
- **Evolution API** — WhatsApp integration
- **Docker Compose** — home server stack with Caddy reverse proxy

See [docs/architecture.md](docs/architecture.md) for details.

## Quick Start

### 1. Install Docker

Install [Docker](https://docs.docker.com/get-docker/) and Docker Compose v2.

### 2. Configure environment

```bash
cp .env.example .env
# Edit .env with secure passwords and your public URL
```

### 3. Start the stack

```bash
cd deploy
docker compose up -d
```

### 4. Run migrations

Migrations run automatically when `tinyvoice-api` starts.

### 5. Configure Evolution API

1. Open Evolution manager at `http://localhost/manager` (or API at `http://localhost/evolution/`)
2. Create instance named `tinyvoice` (or match `EVOLUTION_INSTANCE` in `.env`)
3. Scan QR code with the dedicated WhatsApp account

### 6. Create a device and conversation

```bash
export ADMIN_TOKEN=change-me-admin-token
# Use localhost (not postgres) when running the CLI on your host machine
export DATABASE_URL=postgres://tinyvoice:change-me-postgres@localhost:5432/tinyvoice?sslmode=disable

# Build CLI locally or use docker exec:
cd backend && go run ./cmd/tinyvoice device create --name "Caixa do João"
cd backend && go run ./cmd/tinyvoice conversation create --name "João e Pai" --recipient "5511999999999"
cd backend && go run ./cmd/tinyvoice device bind --device <device-id> --conversation <conversation-id>
```

Save the device token — it is shown only once.

### 7. Configure ESP32

```bash
cp firmware/tinyvoice/include/config.example.h firmware/tinyvoice/include/config.h
# Edit config.h with Wi-Fi, API URL, and device token
cd firmware/tinyvoice
pio run -t upload
```

See [docs/hardware.md](docs/hardware.md) for wiring.

### 8. Test the first audio

1. Power on TinyVoice — LED connects (yellow), then idle (off)
2. Hold the button — LED blue, speak, release — message uploads
3. Parent receives WhatsApp voice note
4. Parent replies with audio
5. TinyVoice LED turns green
6. Press button briefly — audio plays

## Project Structure

```
backend/          Go API, CLI, worker
deploy/           Docker Compose, Caddy
firmware/         ESP32 PlatformIO project
docs/             Documentation
```

## Development

See [docs/development.md](docs/development.md).

## License

MIT
