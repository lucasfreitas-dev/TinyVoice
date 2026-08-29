# Deployment

## Home Server Requirements

- Linux host with Docker Compose v2
- Public domain (optional, for HTTPS via Caddy)
- Ports 80/443 open (if using TLS)

## Install

```bash
git clone <repo-url> tinyvoice
cd tinyvoice
cp .env.example .env
```

Edit `.env`:

| Variable | Description |
|----------|-------------|
| `POSTGRES_PASSWORD` | PostgreSQL password |
| `MINIO_ROOT_PASSWORD` | MinIO password |
| `ADMIN_TOKEN` | CLI admin token |
| `EVOLUTION_API_KEY` | Evolution API key |
| `EVOLUTION_INSTANCE` | WhatsApp instance name |
| `EVOLUTION_WEBHOOK_SECRET` | Webhook auth header |
| `TINYVOICE_PUBLIC_URL` | Public URL for Evolution |

## Start Stack

```bash
cd deploy
chmod +x init-db.sh
docker compose up -d
docker compose logs -f tinyvoice-api
```

## Caddy / HTTPS

Edit `deploy/Caddyfile` with your domain:

```
tinyvoice.example.com {
    reverse_proxy /api/* tinyvoice-api:8080
    reverse_proxy /health tinyvoice-api:8080
    handle_path /evolution/* {
        reverse_proxy evolution:8080
    }
}
```

Caddy obtains TLS certificates automatically.

## Evolution WhatsApp Setup

1. Access Evolution at `https://your-domain/evolution/` (or port 8080 internally)
2. Create instance matching `EVOLUTION_INSTANCE`
3. Set webhook URL: `https://your-domain/api/v1/webhooks/evolution`
4. Enable event: `MESSAGES_UPSERT`
5. Scan QR with dedicated WhatsApp account

## Volumes

Persistent data:

- `postgres_data` — PostgreSQL (tinyvoice + evolution databases)
- `minio_data` — audio files
- `evolution_data` — WhatsApp session
- `caddy_data` — TLS certificates

## Backup

Backup PostgreSQL and MinIO volumes regularly:

```bash
docker compose exec postgres pg_dump -U tinyvoice tinyvoice > backup.sql
```

## Updates

```bash
cd deploy
docker compose pull
docker compose build tinyvoice-api
docker compose up -d
```

Migrations run automatically on API startup.
