# Deployment

## Home Server (local network)

For development or a LAN-only setup, use the default `deploy/Caddyfile` (`:80` without TLS).

```bash
git clone <repo-url> tinyvoice
cd tinyvoice
cp .env.example .env
cd deploy
docker compose --env-file ../.env up -d
```

Set `TINYVOICE_PUBLIC_URL` and `EVOLUTION_PUBLIC_URL` to your LAN IP (e.g. `http://192.168.x.x`).

---

## VPS (production)

Guide for deploying on a public VPS with HTTPS. Use a **subdomain** (e.g. `tinyvoice.example.com`) so you can keep an existing website or email on the apex domain.

Replace `tinyvoice.example.com` and `your-vps-ip` with your values throughout this section.

### Prerequisites

| Item | Example |
|------|---------|
| VPS | 2 vCPU, 4 GB RAM, 40 GB SSD |
| OS | Ubuntu 22.04 or 24.04 |
| Domain | `tinyvoice.example.com` |
| DNS | `A` record `tinyvoice` → VPS public IP |
| WhatsApp | Dedicated number (do not use your personal account) |

Verify DNS before continuing:

```bash
dig tinyvoice.example.com +short
# must return your VPS IP
```

### 1. Initial VPS access

```bash
ssh root@your-vps-ip
```

Create a non-root user:

```bash
adduser deploy
usermod -aG sudo deploy
rsync --archive --chown=deploy:deploy ~/.ssh /home/deploy/
```

Reconnect as `deploy`:

```bash
ssh deploy@your-vps-ip
```

### 2. Firewall

Only SSH and HTTP/HTTPS should be public. Everything else stays on the Docker network.

```bash
sudo ufw default deny incoming
sudo ufw default allow outgoing
sudo ufw allow OpenSSH
sudo ufw allow 80/tcp
sudo ufw allow 443/tcp
sudo ufw enable
sudo ufw status
```

### 3. Install Docker

```bash
sudo apt update && sudo apt upgrade -y
sudo apt install -y git curl
curl -fsSL https://get.docker.com | sh
sudo usermod -aG docker deploy
```

Log out and back in so the `docker` group applies.

### 4. Clone the project

```bash
cd ~
git clone <repo-url> tinyvoice
cd tinyvoice
cp .env.example .env
```

### 5. Configure `.env`

Generate strong secrets:

```bash
openssl rand -hex 32   # use for ADMIN_TOKEN, EVOLUTION_API_KEY, EVOLUTION_WEBHOOK_SECRET
openssl rand -hex 16   # use for POSTGRES_PASSWORD, MINIO_ROOT_PASSWORD
```

Edit `.env`:

```bash
TINYVOICE_PUBLIC_URL=https://tinyvoice.example.com
EVOLUTION_PUBLIC_URL=https://tinyvoice.example.com
EVOLUTION_INSTANCE=tinyvoice
# POSTGRES_PASSWORD, MINIO_ROOT_PASSWORD, ADMIN_TOKEN,
# EVOLUTION_API_KEY, EVOLUTION_WEBHOOK_SECRET — unique random values
```

### 6. Configure Caddy (HTTPS)

Edit `deploy/Caddyfile.prod` — replace `tinyvoice.example.com` with your domain — then copy it:

```bash
cd deploy
cp Caddyfile.prod Caddyfile
```

Caddy obtains Let's Encrypt certificates automatically once DNS resolves and ports 80/443 are open.

### 7. Start the stack

Use the production compose override (keeps PostgreSQL off the public internet):

```bash
cd deploy
chmod +x init-db.sh
docker compose --env-file ../.env -f docker-compose.yml -f docker-compose.prod.yml up -d --build
docker compose --env-file ../.env logs -f tinyvoice-api
```

Check health:

```bash
curl https://tinyvoice.example.com/health
```

### 8. Connect WhatsApp

Evolution manager is at `https://tinyvoice.example.com/manager`.

**Option A — Browser:**

Open the manager URL and scan the QR code with your dedicated WhatsApp account.

**Option B — QR via API (terminal only):**

```bash
cd ~/tinyvoice/deploy
source ../.env

curl -s "https://tinyvoice.example.com/instance/connect/${EVOLUTION_INSTANCE}" \
  -H "apikey: $EVOLUTION_API_KEY" | jq -r '.base64' | base64 -d > /tmp/qr.png

scp deploy@your-vps-ip:/tmp/qr.png ~/Downloads/evolution-qr.png
```

Poll until connected:

```bash
curl -s "https://tinyvoice.example.com/instance/connectionState/${EVOLUTION_INSTANCE}" \
  -H "apikey: $EVOLUTION_API_KEY" | jq .instance.state
# wait for "open"
```

### 9. Configure Evolution webhook

The webhook must use the **internal** Docker URL (Evolution and tinyvoice-api share a network). Use the nested `webhook` object required by Evolution v2.3.6:

```bash
cd ~/tinyvoice/deploy
source ../.env

curl -X POST "https://tinyvoice.example.com/webhook/set/${EVOLUTION_INSTANCE}" \
  -H "apikey: ${EVOLUTION_API_KEY}" \
  -H "Content-Type: application/json" \
  -d "{
    \"webhook\": {
      \"enabled\": true,
      \"url\": \"http://tinyvoice-api:8080/api/v1/webhooks/evolution\",
      \"events\": [\"MESSAGES_UPSERT\"],
      \"headers\": {\"apikey\": \"${EVOLUTION_WEBHOOK_SECRET}\"}
    }
  }"
```

Verify:

```bash
curl -s "https://tinyvoice.example.com/webhook/find/${EVOLUTION_INSTANCE}" \
  -H "apikey: ${EVOLUTION_API_KEY}" | jq .
```

### 10. Create device and conversation

Run CLI inside the API container:

```bash
docker compose --env-file ../.env exec tinyvoice-api \
  tinyvoice device create --name "My Box"

docker compose --env-file ../.env exec tinyvoice-api \
  tinyvoice conversation create --name "Family" --recipient "5511000000001"

docker compose --env-file ../.env exec tinyvoice-api \
  tinyvoice device bind --device <device-id> --conversation <conversation-id>
```

Save the device token — it is shown only once.

### 11. Configure ESP32

```bash
cp firmware/tinyvoice/include/config.example.h firmware/tinyvoice/include/config.h
```

Set in `config.h`:

```c
#define API_BASE_URL "https://tinyvoice.example.com"
#define DEVICE_ID "<device-id>"
#define DEVICE_TOKEN "<device-token>"
```

Flash:

```bash
cd firmware/tinyvoice
pio run -t upload
```

> **HTTPS note:** Production firmware currently uses `WiFiClient` (HTTP only). Before pointing the ESP32 at the HTTPS URL, add `WiFiClientSecure` support in `api_client.cpp`.

### 12. End-to-end test

1. ESP records and uploads → parent receives WhatsApp voice note
2. Parent replies with audio → webhook fires → ESP LED turns green
3. Monitor logs:

```bash
docker compose --env-file ../.env logs -f tinyvoice-api | grep -E "message_received|webhook|whatsapp"
```

---

## DNS setup

If your apex domain already hosts a website or email, **do not** point it at the VPS. Add a subdomain instead:

```
Type:  A
Host:  tinyvoice          (or any label you prefer)
Value: <your-vps-public-ip>
```

Then use `tinyvoice.yourdomain.com` everywhere (`.env`, `Caddyfile.prod`, ESP32 `config.h`).

Leave existing records for your website (`@`, `www`) and mail (`MX`, SPF, DKIM) unchanged.

---

## Caddy / HTTPS (reference)

Local dev uses `:80` in `deploy/Caddyfile`. Production uses `deploy/Caddyfile.prod`:

```
tinyvoice.example.com {
    handle /api/* { reverse_proxy tinyvoice-api:8080 }
    handle /health { reverse_proxy tinyvoice-api:8080 }
    handle /manager/* { reverse_proxy evolution:8080 }
    ...
}
```

---

## Volumes

Persistent data:

- `postgres_data` — PostgreSQL (tinyvoice + evolution databases)
- `minio_data` — audio files
- `evolution_data` — WhatsApp session
- `caddy_data` — TLS certificates

## Backup

```bash
docker compose exec postgres pg_dump -U tinyvoice tinyvoice > backup.sql
```

Back up `evolution_data` and `minio_data` volumes regularly — losing them disconnects WhatsApp and deletes audio.

## Updates

```bash
cd deploy
git pull
docker compose --env-file ../.env -f docker-compose.yml -f docker-compose.prod.yml build tinyvoice-api
docker compose --env-file ../.env -f docker-compose.yml -f docker-compose.prod.yml up -d
```

Migrations run automatically on API startup.
