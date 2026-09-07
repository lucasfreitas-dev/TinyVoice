# API Reference

Base URL: `https://your-domain.com`

## Health

```
GET /health
```

Response:
```json
{"status": "ok"}
```

## Device Authentication

All `/api/v1/device/*` endpoints require:

```
Authorization: Bearer <device_token>
```

## Heartbeat

```
POST /api/v1/device/heartbeat
```

Response:
```json
{"status": "ok"}
```

## Upload Message

```
POST /api/v1/device/messages
Content-Type: multipart/form-data
```

Form field: `audio` (WAV file, mono 16 kHz, 500ms–60s)

Response `201`:
```json
{"id": "uuid", "status": "pending"}
```

## Poll Next Message

```
GET /api/v1/device/messages/next
```

No message:
```json
{"available": false}
```

Message available:
```json
{
  "available": true,
  "id": "uuid",
  "duration_ms": 12500,
  "size_bytes": 52341
}
```

## Download Audio

```
GET /api/v1/device/messages/{id}/audio
```

Returns audio stream (`audio/wav` for inbound).

## Mark Played

```
POST /api/v1/device/messages/{id}/played
```

Response:
```json
{"status": "played"}
```

## Evolution Webhook (WhatsApp)

```
POST /api/v1/webhooks/evolution
apikey: <EVOLUTION_WEBHOOK_SECRET or EVOLUTION_API_KEY>
```

Configure Evolution instance webhook for `MESSAGES_UPSERT` pointing to this URL.

Only inbound audio messages are processed. Duplicate events are ignored via idempotency table.

## Telegram Webhook

```
POST /api/v1/webhooks/telegram
X-Telegram-Bot-Api-Secret-Token: <TELEGRAM_WEBHOOK_SECRET>
```

Telegram Bot API updates. Only `message.voice` and `message.audio` are processed. The conversation recipient must be the Telegram chat id (numeric string).

If `TELEGRAM_BOT_TOKEN` is set and `TINYVOICE_PUBLIC_URL` is HTTPS (or localhost), the API registers this webhook on startup.
