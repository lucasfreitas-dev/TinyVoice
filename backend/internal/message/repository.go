package message

import (
	"context"
	"fmt"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

const (
	DirectionOutbound = "OUTBOUND"
	DirectionInbound  = "INBOUND"

	StatusPending    = "PENDING"
	StatusProcessing = "PROCESSING"
	StatusSent       = "SENT"
	StatusAvailable  = "AVAILABLE"
	StatusPlayed     = "PLAYED"
	StatusFailed     = "FAILED"
)

type Message struct {
	ID               string
	ConversationID   string
	DeviceID         string
	Direction        string
	Status           string
	AudioStorageKey  *string
	AudioDurationMs  *int
	AudioSizeBytes   *int64
	MimeType         *string
	Attempts         int
	LastError        *string
	ExternalID       *string
	CreatedAt        time.Time
	SentAt           *time.Time
	ReceivedAt       *time.Time
	PlayedAt         *time.Time
}

type Repository struct {
	pool *pgxpool.Pool
}

func NewRepository(pool *pgxpool.Pool) *Repository {
	return &Repository{pool: pool}
}

func (r *Repository) Create(ctx context.Context, m *Message) (*Message, error) {
	const q = `
		INSERT INTO messages (
			conversation_id, device_id, direction, status,
			audio_storage_key, audio_duration_ms, audio_size_bytes, mime_type, external_id
		) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)
		RETURNING id, conversation_id, device_id, direction, status,
			audio_storage_key, audio_duration_ms, audio_size_bytes, mime_type,
			attempts, last_error, external_id, created_at, sent_at, received_at, played_at
	`
	var out Message
	err := r.pool.QueryRow(ctx, q,
		m.ConversationID, m.DeviceID, m.Direction, m.Status,
		m.AudioStorageKey, m.AudioDurationMs, m.AudioSizeBytes, m.MimeType, m.ExternalID,
	).Scan(
		&out.ID, &out.ConversationID, &out.DeviceID, &out.Direction, &out.Status,
		&out.AudioStorageKey, &out.AudioDurationMs, &out.AudioSizeBytes, &out.MimeType,
		&out.Attempts, &out.LastError, &out.ExternalID,
		&out.CreatedAt, &out.SentAt, &out.ReceivedAt, &out.PlayedAt,
	)
	if err != nil {
		return nil, fmt.Errorf("insert message: %w", err)
	}
	return &out, nil
}

func (r *Repository) GetByID(ctx context.Context, id string) (*Message, error) {
	const q = `
		SELECT id, conversation_id, device_id, direction, status,
			audio_storage_key, audio_duration_ms, audio_size_bytes, mime_type,
			attempts, last_error, external_id, created_at, sent_at, received_at, played_at
		FROM messages WHERE id = $1
	`
	var m Message
	err := r.pool.QueryRow(ctx, q, id).Scan(
		&m.ID, &m.ConversationID, &m.DeviceID, &m.Direction, &m.Status,
		&m.AudioStorageKey, &m.AudioDurationMs, &m.AudioSizeBytes, &m.MimeType,
		&m.Attempts, &m.LastError, &m.ExternalID,
		&m.CreatedAt, &m.SentAt, &m.ReceivedAt, &m.PlayedAt,
	)
	if err != nil {
		return nil, fmt.Errorf("get message: %w", err)
	}
	return &m, nil
}

func (r *Repository) GetNextAvailable(ctx context.Context, deviceID string) (*Message, error) {
	const q = `
		SELECT id, conversation_id, device_id, direction, status,
			audio_storage_key, audio_duration_ms, audio_size_bytes, mime_type,
			attempts, last_error, external_id, created_at, sent_at, received_at, played_at
		FROM messages
		WHERE device_id = $1 AND status = 'AVAILABLE'
		ORDER BY created_at ASC
		LIMIT 1
	`
	var m Message
	err := r.pool.QueryRow(ctx, q, deviceID).Scan(
		&m.ID, &m.ConversationID, &m.DeviceID, &m.Direction, &m.Status,
		&m.AudioStorageKey, &m.AudioDurationMs, &m.AudioSizeBytes, &m.MimeType,
		&m.Attempts, &m.LastError, &m.ExternalID,
		&m.CreatedAt, &m.SentAt, &m.ReceivedAt, &m.PlayedAt,
	)
	if err != nil {
		if err == pgx.ErrNoRows {
			return nil, nil
		}
		return nil, fmt.Errorf("get next available: %w", err)
	}
	return &m, nil
}

func (r *Repository) MarkPlayed(ctx context.Context, id, deviceID string) error {
	const q = `
		UPDATE messages SET status = 'PLAYED', played_at = NOW()
		WHERE id = $1 AND device_id = $2 AND status = 'AVAILABLE'
	`
	tag, err := r.pool.Exec(ctx, q, id, deviceID)
	if err != nil {
		return fmt.Errorf("mark played: %w", err)
	}
	if tag.RowsAffected() == 0 {
		return fmt.Errorf("message not found or not available")
	}
	return nil
}

func (r *Repository) ClaimPending(ctx context.Context) (*Message, error) {
	tx, err := r.pool.Begin(ctx)
	if err != nil {
		return nil, fmt.Errorf("begin tx: %w", err)
	}
	defer tx.Rollback(ctx)

	const q = `
		SELECT id, conversation_id, device_id, direction, status,
			audio_storage_key, audio_duration_ms, audio_size_bytes, mime_type,
			attempts, last_error, external_id, created_at, sent_at, received_at, played_at
		FROM messages
		WHERE status = 'PENDING'
		ORDER BY created_at ASC
		FOR UPDATE SKIP LOCKED
		LIMIT 1
	`
	var m Message
	err = tx.QueryRow(ctx, q).Scan(
		&m.ID, &m.ConversationID, &m.DeviceID, &m.Direction, &m.Status,
		&m.AudioStorageKey, &m.AudioDurationMs, &m.AudioSizeBytes, &m.MimeType,
		&m.Attempts, &m.LastError, &m.ExternalID,
		&m.CreatedAt, &m.SentAt, &m.ReceivedAt, &m.PlayedAt,
	)
	if err != nil {
		if err == pgx.ErrNoRows {
			return nil, nil
		}
		return nil, fmt.Errorf("claim pending: %w", err)
	}

	const upd = `UPDATE messages SET status = 'PROCESSING', attempts = attempts + 1 WHERE id = $1`
	if _, err := tx.Exec(ctx, upd, m.ID); err != nil {
		return nil, fmt.Errorf("mark processing: %w", err)
	}

	if err := tx.Commit(ctx); err != nil {
		return nil, fmt.Errorf("commit: %w", err)
	}
	m.Status = StatusProcessing
	m.Attempts++
	return &m, nil
}

func (r *Repository) MarkSent(ctx context.Context, id string) error {
	const q = `UPDATE messages SET status = 'SENT', sent_at = NOW() WHERE id = $1`
	_, err := r.pool.Exec(ctx, q, id)
	return err
}

func (r *Repository) MarkFailed(ctx context.Context, id, errMsg string) error {
	const q = `UPDATE messages SET status = 'FAILED', last_error = $2 WHERE id = $1`
	_, err := r.pool.Exec(ctx, q, id, errMsg)
	return err
}

func (r *Repository) RequeuePending(ctx context.Context, id, errMsg string) error {
	const q = `UPDATE messages SET status = 'PENDING', last_error = $2 WHERE id = $1`
	_, err := r.pool.Exec(ctx, q, id, errMsg)
	return err
}

func (r *Repository) ExistsByExternalID(ctx context.Context, externalID string) (bool, error) {
	const q = `SELECT EXISTS(SELECT 1 FROM messages WHERE external_id = $1)`
	var exists bool
	err := r.pool.QueryRow(ctx, q, externalID).Scan(&exists)
	return exists, err
}

func (r *Repository) RecordWebhookEvent(ctx context.Context, provider, externalID string) (bool, error) {
	const q = `
		INSERT INTO webhook_events (provider, external_id)
		VALUES ($1, $2)
		ON CONFLICT (external_id) DO NOTHING
		RETURNING id
	`
	var id string
	err := r.pool.QueryRow(ctx, q, provider, externalID).Scan(&id)
	if err != nil {
		if err == pgx.ErrNoRows {
			return false, nil
		}
		return false, fmt.Errorf("record webhook event: %w", err)
	}
	return true, nil
}
