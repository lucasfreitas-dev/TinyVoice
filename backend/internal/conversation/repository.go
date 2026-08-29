package conversation

import (
	"context"
	"fmt"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"
)

type Conversation struct {
	ID                 string
	Name               string
	WhatsAppRecipient  string
	CreatedAt          time.Time
	UpdatedAt          time.Time
}

type Repository struct {
	pool *pgxpool.Pool
}

func NewRepository(pool *pgxpool.Pool) *Repository {
	return &Repository{pool: pool}
}

func (r *Repository) Create(ctx context.Context, name, recipient string) (*Conversation, error) {
	const q = `
		INSERT INTO conversations (name, whatsapp_recipient)
		VALUES ($1, $2)
		RETURNING id, name, whatsapp_recipient, created_at, updated_at
	`
	var c Conversation
	err := r.pool.QueryRow(ctx, q, name, recipient).Scan(
		&c.ID, &c.Name, &c.WhatsAppRecipient, &c.CreatedAt, &c.UpdatedAt,
	)
	if err != nil {
		return nil, fmt.Errorf("insert conversation: %w", err)
	}
	return &c, nil
}

func (r *Repository) GetByID(ctx context.Context, id string) (*Conversation, error) {
	const q = `
		SELECT id, name, whatsapp_recipient, created_at, updated_at
		FROM conversations WHERE id = $1
	`
	var c Conversation
	err := r.pool.QueryRow(ctx, q, id).Scan(
		&c.ID, &c.Name, &c.WhatsAppRecipient, &c.CreatedAt, &c.UpdatedAt,
	)
	if err != nil {
		return nil, fmt.Errorf("get conversation: %w", err)
	}
	return &c, nil
}

func (r *Repository) List(ctx context.Context) ([]Conversation, error) {
	const q = `
		SELECT id, name, whatsapp_recipient, created_at, updated_at
		FROM conversations ORDER BY created_at DESC
	`
	rows, err := r.pool.Query(ctx, q)
	if err != nil {
		return nil, fmt.Errorf("list conversations: %w", err)
	}
	defer rows.Close()

	var items []Conversation
	for rows.Next() {
		var c Conversation
		if err := rows.Scan(&c.ID, &c.Name, &c.WhatsAppRecipient, &c.CreatedAt, &c.UpdatedAt); err != nil {
			return nil, fmt.Errorf("scan conversation: %w", err)
		}
		items = append(items, c)
	}
	return items, rows.Err()
}
