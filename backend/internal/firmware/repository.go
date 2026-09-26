package firmware

import (
	"context"
	"errors"
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

func (r *Repository) Upsert(ctx context.Context, version, storageKey, sha256 string, sizeBytes int64) (*Release, error) {
	const q = `
		INSERT INTO firmware_releases (version, storage_key, sha256, size_bytes)
		VALUES ($1, $2, $3, $4)
		ON CONFLICT (version) DO UPDATE SET
			storage_key = EXCLUDED.storage_key,
			sha256 = EXCLUDED.sha256,
			size_bytes = EXCLUDED.size_bytes
		RETURNING id, version, storage_key, sha256, size_bytes, created_at
	`
	var rel Release
	err := r.pool.QueryRow(ctx, q, version, storageKey, sha256, sizeBytes).Scan(
		&rel.ID, &rel.Version, &rel.StorageKey, &rel.SHA256, &rel.SizeBytes, &rel.CreatedAt,
	)
	if err != nil {
		return nil, fmt.Errorf("upsert firmware: %w", err)
	}
	return &rel, nil
}

func (r *Repository) List(ctx context.Context) ([]Release, error) {
	const q = `
		SELECT id, version, storage_key, sha256, size_bytes, created_at
		FROM firmware_releases
		ORDER BY created_at DESC
	`
	rows, err := r.pool.Query(ctx, q)
	if err != nil {
		return nil, fmt.Errorf("list firmware: %w", err)
	}
	defer rows.Close()

	var out []Release
	for rows.Next() {
		var rel Release
		if err := rows.Scan(&rel.ID, &rel.Version, &rel.StorageKey, &rel.SHA256, &rel.SizeBytes, &rel.CreatedAt); err != nil {
			return nil, fmt.Errorf("scan firmware: %w", err)
		}
		out = append(out, rel)
	}
	return out, rows.Err()
}

func (r *Repository) GetByVersion(ctx context.Context, version string) (*Release, error) {
	const q = `
		SELECT id, version, storage_key, sha256, size_bytes, created_at
		FROM firmware_releases
		WHERE version = $1
	`
	var rel Release
	err := r.pool.QueryRow(ctx, q, version).Scan(
		&rel.ID, &rel.Version, &rel.StorageKey, &rel.SHA256, &rel.SizeBytes, &rel.CreatedAt,
	)
	if errors.Is(err, pgx.ErrNoRows) {
		return nil, fmt.Errorf("firmware version %q not found", version)
	}
	if err != nil {
		return nil, fmt.Errorf("get firmware: %w", err)
	}
	return &rel, nil
}

func (r *Repository) DesiredForDevice(ctx context.Context, deviceID string) (*Release, error) {
	const q = `
		SELECT f.id, f.version, f.storage_key, f.sha256, f.size_bytes, f.created_at
		FROM device_runtime r
		JOIN firmware_releases f ON f.id = r.desired_firmware_id
		WHERE r.device_id = $1
	`
	var rel Release
	err := r.pool.QueryRow(ctx, q, deviceID).Scan(
		&rel.ID, &rel.Version, &rel.StorageKey, &rel.SHA256, &rel.SizeBytes, &rel.CreatedAt,
	)
	if errors.Is(err, pgx.ErrNoRows) {
		return nil, nil
	}
	if err != nil {
		return nil, fmt.Errorf("desired firmware: %w", err)
	}
	return &rel, nil
}

func (r *Repository) Assign(ctx context.Context, deviceID, firmwareID string) error {
	const q = `
		INSERT INTO device_runtime (device_id, desired_firmware_id, updated_at)
		VALUES ($1, $2, $3)
		ON CONFLICT (device_id) DO UPDATE SET
			desired_firmware_id = EXCLUDED.desired_firmware_id,
			updated_at = EXCLUDED.updated_at
	`
	_, err := r.pool.Exec(ctx, q, deviceID, firmwareID, time.Now().UTC())
	if err != nil {
		return fmt.Errorf("assign firmware: %w", err)
	}
	return nil
}

func (r *Repository) AssignAll(ctx context.Context, firmwareID string) (int64, error) {
	const q = `
		INSERT INTO device_runtime (device_id, desired_firmware_id, updated_at)
		SELECT id, $1, $2 FROM devices
		ON CONFLICT (device_id) DO UPDATE SET
			desired_firmware_id = EXCLUDED.desired_firmware_id,
			updated_at = EXCLUDED.updated_at
	`
	tag, err := r.pool.Exec(ctx, q, firmwareID, time.Now().UTC())
	if err != nil {
		return 0, fmt.Errorf("assign firmware to all: %w", err)
	}
	return tag.RowsAffected(), nil
}

func (r *Repository) Clear(ctx context.Context, deviceID string) error {
	const q = `
		UPDATE device_runtime
		SET desired_firmware_id = NULL, updated_at = $2
		WHERE device_id = $1
	`
	_, err := r.pool.Exec(ctx, q, deviceID, time.Now().UTC())
	if err != nil {
		return fmt.Errorf("clear firmware assignment: %w", err)
	}
	return nil
}

func (r *Repository) DeviceExists(ctx context.Context, deviceID string) (bool, error) {
	const q = `SELECT EXISTS(SELECT 1 FROM devices WHERE id = $1)`
	var ok bool
	if err := r.pool.QueryRow(ctx, q, deviceID).Scan(&ok); err != nil {
		return false, fmt.Errorf("device exists: %w", err)
	}
	return ok, nil
}
