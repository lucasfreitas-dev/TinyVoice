package device

import (
	"context"
	"fmt"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

type Repository struct {
	pool *pgxpool.Pool
}

func NewRepository(pool *pgxpool.Pool) *Repository {
	return &Repository{pool: pool}
}

func (r *Repository) Create(ctx context.Context, name, tokenHash string) (*Device, error) {
	const q = `
		INSERT INTO devices (name, token_hash)
		VALUES ($1, $2)
		RETURNING id, name, token_hash, enabled, created_at, updated_at, last_seen_at
	`
	var d Device
	err := r.pool.QueryRow(ctx, q, name, tokenHash).Scan(
		&d.ID, &d.Name, &d.TokenHash, &d.Enabled, &d.CreatedAt, &d.UpdatedAt, &d.LastSeenAt,
	)
	if err != nil {
		return nil, fmt.Errorf("insert device: %w", err)
	}
	return &d, nil
}

func (r *Repository) List(ctx context.Context) ([]Device, error) {
	const q = `
		SELECT d.id, d.name, d.token_hash, d.enabled, d.created_at, d.updated_at, d.last_seen_at,
		       COALESCE(rt.firmware_version, '')
		FROM devices d
		LEFT JOIN device_runtime rt ON rt.device_id = d.id
		ORDER BY d.created_at DESC
	`
	rows, err := r.pool.Query(ctx, q)
	if err != nil {
		return nil, fmt.Errorf("list devices: %w", err)
	}
	defer rows.Close()

	var devices []Device
	for rows.Next() {
		var d Device
		if err := rows.Scan(&d.ID, &d.Name, &d.TokenHash, &d.Enabled, &d.CreatedAt, &d.UpdatedAt, &d.LastSeenAt, &d.FirmwareVersion); err != nil {
			return nil, fmt.Errorf("scan device: %w", err)
		}
		devices = append(devices, d)
	}
	return devices, rows.Err()
}

func (r *Repository) GetByID(ctx context.Context, id string) (*Device, error) {
	const q = `
		SELECT id, name, token_hash, enabled, created_at, updated_at, last_seen_at
		FROM devices WHERE id = $1
	`
	var d Device
	err := r.pool.QueryRow(ctx, q, id).Scan(
		&d.ID, &d.Name, &d.TokenHash, &d.Enabled, &d.CreatedAt, &d.UpdatedAt, &d.LastSeenAt,
	)
	if err != nil {
		return nil, fmt.Errorf("get device: %w", err)
	}
	return &d, nil
}

func (r *Repository) Authenticate(ctx context.Context, token string) (*Device, error) {
	const q = `
		SELECT id, name, token_hash, enabled, created_at, updated_at, last_seen_at
		FROM devices WHERE enabled = true
	`
	rows, err := r.pool.Query(ctx, q)
	if err != nil {
		return nil, fmt.Errorf("authenticate query: %w", err)
	}
	defer rows.Close()

	for rows.Next() {
		var d Device
		if err := rows.Scan(&d.ID, &d.Name, &d.TokenHash, &d.Enabled, &d.CreatedAt, &d.UpdatedAt, &d.LastSeenAt); err != nil {
			return nil, fmt.Errorf("scan device: %w", err)
		}
		if VerifyToken(token, d.TokenHash) {
			return &d, nil
		}
	}
	return nil, fmt.Errorf("invalid token")
}

func (r *Repository) UpdateLastSeen(ctx context.Context, id string) error {
	const q = `UPDATE devices SET last_seen_at = $2, updated_at = $2 WHERE id = $1`
	now := time.Now().UTC()
	_, err := r.pool.Exec(ctx, q, id, now)
	if err != nil {
		return fmt.Errorf("update last seen: %w", err)
	}
	return nil
}

func (r *Repository) BindConversation(ctx context.Context, deviceID, conversationID string) error {
	const q = `
		INSERT INTO device_conversations (device_id, conversation_id)
		VALUES ($1, $2)
		ON CONFLICT DO NOTHING
	`
	_, err := r.pool.Exec(ctx, q, deviceID, conversationID)
	if err != nil {
		return fmt.Errorf("bind conversation: %w", err)
	}
	return nil
}

func (r *Repository) FindByRecipient(ctx context.Context, channel, recipient string) (deviceID, conversationID string, err error) {
	const q = `
		SELECT dc.device_id, dc.conversation_id
		FROM device_conversations dc
		JOIN conversations c ON c.id = dc.conversation_id
		WHERE c.channel = $1 AND c.recipient = $2
		LIMIT 1
	`
	err = r.pool.QueryRow(ctx, q, channel, recipient).Scan(&deviceID, &conversationID)
	if err != nil {
		return "", "", fmt.Errorf("find by recipient: %w", err)
	}
	return deviceID, conversationID, nil
}

func (r *Repository) UpsertRuntime(ctx context.Context, deviceID string, in RuntimeUpdate) error {
	const q = `
		INSERT INTO device_runtime (
			device_id, firmware_version, last_rssi, last_free_heap, last_uptime_ms, updated_at
		)
		VALUES ($1, NULLIF($2, ''), $3, $4, $5, $6)
		ON CONFLICT (device_id) DO UPDATE SET
			firmware_version = COALESCE(NULLIF(EXCLUDED.firmware_version, ''), device_runtime.firmware_version),
			last_rssi = COALESCE(EXCLUDED.last_rssi, device_runtime.last_rssi),
			last_free_heap = COALESCE(EXCLUDED.last_free_heap, device_runtime.last_free_heap),
			last_uptime_ms = COALESCE(EXCLUDED.last_uptime_ms, device_runtime.last_uptime_ms),
			updated_at = EXCLUDED.updated_at
	`
	now := time.Now().UTC()
	_, err := r.pool.Exec(ctx, q, deviceID, in.FirmwareVersion, in.RSSI, in.FreeHeap, in.UptimeMs, now)
	if err != nil {
		return fmt.Errorf("upsert device runtime: %w", err)
	}
	return nil
}

func (r *Repository) InsertLogs(ctx context.Context, deviceID, firmwareVersion string, entries []LogIn) error {
	if len(entries) == 0 {
		return nil
	}
	const q = `
		INSERT INTO device_logs (device_id, ts_ms, level, message, firmware_version)
		VALUES ($1, $2, $3, $4, NULLIF($5, ''))
	`
	batch := &pgx.Batch{}
	for _, e := range entries {
		batch.Queue(q, deviceID, e.TsMs, e.Level, e.Message, firmwareVersion)
	}
	br := r.pool.SendBatch(ctx, batch)
	defer br.Close()
	for range entries {
		if _, err := br.Exec(); err != nil {
			return fmt.Errorf("insert device log: %w", err)
		}
	}

	const trim = `
		DELETE FROM device_logs
		WHERE device_id = $1
		  AND id NOT IN (
			SELECT id FROM device_logs
			WHERE device_id = $1
			ORDER BY created_at DESC
			LIMIT $2
		  )
	`
	if _, err := r.pool.Exec(ctx, trim, deviceID, MaxLogsPerDevice); err != nil {
		return fmt.Errorf("trim device logs: %w", err)
	}
	return nil
}

func (r *Repository) ListLogs(ctx context.Context, deviceID string, limit int) ([]LogEntry, error) {
	if limit <= 0 || limit > 500 {
		limit = 100
	}
	const q = `
		SELECT id, device_id, ts_ms, level, message, COALESCE(firmware_version, ''), created_at
		FROM device_logs
		WHERE device_id = $1
		ORDER BY created_at DESC
		LIMIT $2
	`
	rows, err := r.pool.Query(ctx, q, deviceID, limit)
	if err != nil {
		return nil, fmt.Errorf("list device logs: %w", err)
	}
	defer rows.Close()

	var out []LogEntry
	for rows.Next() {
		var e LogEntry
		if err := rows.Scan(&e.ID, &e.DeviceID, &e.TsMs, &e.Level, &e.Message, &e.FirmwareVersion, &e.CreatedAt); err != nil {
			return nil, fmt.Errorf("scan device log: %w", err)
		}
		out = append(out, e)
	}
	return out, rows.Err()
}

func (r *Repository) GetConversationID(ctx context.Context, deviceID string) (string, error) {
	const q = `
		SELECT conversation_id FROM device_conversations
		WHERE device_id = $1 LIMIT 1
	`
	var id string
	err := r.pool.QueryRow(ctx, q, deviceID).Scan(&id)
	if err != nil {
		return "", fmt.Errorf("get conversation for device: %w", err)
	}
	return id, nil
}
