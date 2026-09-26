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
Content-Type: application/json
```

Body is optional. Empty body still updates last-seen (older firmware).

```json
{
  "firmware_version": "0.1.0",
  "uptime_ms": 125000,
  "free_heap": 81234,
  "rssi": -58,
  "logs": [
    {"ts_ms": 1200, "level": "info", "msg": "wifi: connected"}
  ]
}
```

Response:
```json
{
  "status": "ok",
  "firmware": {
    "update_available": false
  }
}
```

When an admin has assigned a different firmware version to the device:

```json
{
  "status": "ok",
  "firmware": {
    "update_available": true,
    "version": "0.2.0",
    "size_bytes": 987654,
    "sha256": "…"
  },
  "logs": {"accepted": 1}
}
```

## Remote Logs

```
POST /api/v1/device/logs
Content-Type: application/json
```

Used when the on-device ring buffer is nearly full between heartbeats.

```json
{
  "firmware_version": "0.1.0",
  "uptime_ms": 125000,
  "entries": [
    {"ts_ms": 1200, "level": "error", "msg": "upload: failed"}
  ]
}
```

Response:
```json
{"status": "ok", "accepted": 1}
```

At most 50 entries per request. Each message is truncated to 256 characters. The API keeps the last 1000 lines per device.

## Firmware Download

```
GET /api/v1/device/firmware/binary
```

Streams the firmware binary currently assigned to this device (`application/octet-stream`).

Headers:

```
Content-Length: 987654
X-Firmware-Version: 0.2.0
X-Firmware-SHA256: …
```

`404` if no firmware is assigned. The device only downloads when heartbeat reports `update_available`.

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
