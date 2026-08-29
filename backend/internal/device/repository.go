package device

import (
	"context"
	"fmt"
	"time"

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
		SELECT id, name, token_hash, enabled, created_at, updated_at, last_seen_at
		FROM devices ORDER BY created_at DESC
	`
	rows, err := r.pool.Query(ctx, q)
	if err != nil {
		return nil, fmt.Errorf("list devices: %w", err)
	}
	defer rows.Close()

	var devices []Device
	for rows.Next() {
		var d Device
		if err := rows.Scan(&d.ID, &d.Name, &d.TokenHash, &d.Enabled, &d.CreatedAt, &d.UpdatedAt, &d.LastSeenAt); err != nil {
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

func (r *Repository) FindByRecipient(ctx context.Context, recipient string) (deviceID, conversationID string, err error) {
	const q = `
		SELECT dc.device_id, dc.conversation_id
		FROM device_conversations dc
		JOIN conversations c ON c.id = dc.conversation_id
		WHERE c.whatsapp_recipient = $1
		LIMIT 1
	`
	err = r.pool.QueryRow(ctx, q, recipient).Scan(&deviceID, &conversationID)
	if err != nil {
		return "", "", fmt.Errorf("find by recipient: %w", err)
	}
	return deviceID, conversationID, nil
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
