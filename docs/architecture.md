# Architecture

## Overview

TinyVoice connects a child-friendly physical device to WhatsApp or Telegram through a self-hosted backend.

```
ESP32 (TinyVoice)
    │ HTTPS
    ▼
Caddy (TLS reverse proxy)
    ▼
TinyVoice API (Go)
    ├── PostgreSQL (devices, messages, queue)
    ├── MinIO (audio files)
    └── Worker → Evolution API → WhatsApp
             └── Telegram Bot API → Telegram
```

## Components

### ESP32 Firmware

- State machine controls all device behavior
- Records mono 16 kHz WAV via INMP441 (I2S)
- Plays WAV via MAX98357A (I2S)
- Polls `GET /api/v1/device/messages/next` every 5 seconds
- Local LittleFS queue for offline uploads

### TinyVoice API

REST API under `/api/v1`:

| Endpoint | Auth | Purpose |
|----------|------|---------|
| `GET /health` | none | Health check |
| `POST /api/v1/device/heartbeat` | device | Update last seen |
| `POST /api/v1/device/messages` | device | Upload audio |
| `GET /api/v1/device/messages/next` | device | Poll for messages |
| `GET /api/v1/device/messages/{id}/audio` | device | Download audio |
| `POST /api/v1/device/messages/{id}/played` | device | Mark played |
| `POST /api/v1/webhooks/evolution` | apikey | Receive WhatsApp audio |
| `POST /api/v1/webhooks/telegram` | `X-Telegram-Bot-Api-Secret-Token` | Receive Telegram voice/audio |

### Worker

Polls PostgreSQL for `PENDING` outbound messages using `FOR UPDATE SKIP LOCKED`. Converts WAV to Opus via FFmpeg, then sends through the conversation's channel provider (Evolution for WhatsApp, Telegram Bot API for Telegram). Updates status to `SENT` or `FAILED` with exponential backoff retry.

### Storage

MinIO bucket `tinyvoice-audio`:

```
audio/outbound/{uuid}
audio/inbound/{uuid}
```

Files are streamed — never loaded fully into memory for HTTP responses.

## Message Flow

### Outbound (child → parent)

1. ESP uploads WAV multipart
2. API validates, stores in MinIO, creates message `PENDING`
3. Worker converts to Opus, sends via the conversation channel (WhatsApp or Telegram)
4. Status → `SENT`

### Inbound (parent → child)

1. Channel webhook (Evolution `MESSAGES_UPSERT` or Telegram `message.voice` / `message.audio`)
2. API downloads media, converts to WAV, stores in MinIO
3. Message status → `AVAILABLE`
4. ESP polls, LED green
5. Child presses button, ESP downloads and plays
6. ESP calls `/played`, status → `PLAYED`

## Security

- Device tokens stored as bcrypt hashes only
- WhatsApp webhook protected by Evolution API key header
- Telegram webhook protected by `X-Telegram-Bot-Api-Secret-Token`
- Admin CLI requires `ADMIN_TOKEN`
- No secrets in structured logs

## Future Extensions

Prepared but not implemented in MVP:

- Captive portal Wi-Fi setup
- Battery / presence sensor / display
- WebSocket push (polling is sufficient for MVP)

Volume pot on GPIO 34 is implemented: ADC reading scales PCM playback, and the minimum gain is never mute.
