package firmware

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"os"

	"tinyvoice/backend/internal/storage"
)

type Service struct {
	repo    *Repository
	storage storage.Provider
}

func NewService(repo *Repository, store storage.Provider) *Service {
	return &Service{repo: repo, storage: store}
}

func (s *Service) Upload(ctx context.Context, version, localPath string) (*Release, error) {
	if err := ValidateVersion(version); err != nil {
		return nil, err
	}

	f, err := os.Open(localPath)
	if err != nil {
		return nil, fmt.Errorf("open firmware: %w", err)
	}
	defer f.Close()

	info, err := f.Stat()
	if err != nil {
		return nil, fmt.Errorf("stat firmware: %w", err)
	}
	if info.Size() <= 0 {
		return nil, fmt.Errorf("firmware file is empty")
	}
	if info.Size() > MaxBinaryBytes {
		return nil, fmt.Errorf("firmware is %d bytes; max for the ESP32 OTA slot is %d", info.Size(), MaxBinaryBytes)
	}

	sum := sha256.New()
	if _, err := io.Copy(sum, f); err != nil {
		return nil, fmt.Errorf("hash firmware: %w", err)
	}
	digest := hex.EncodeToString(sum.Sum(nil))

	if err := s.storage.EnsureBucket(ctx); err != nil {
		return nil, err
	}
	key := StorageKey(version)
	if err := s.storage.PutFile(ctx, key, localPath, "application/octet-stream"); err != nil {
		return nil, err
	}

	return s.repo.Upsert(ctx, version, key, digest, info.Size())
}

func (s *Service) List(ctx context.Context) ([]Release, error) {
	return s.repo.List(ctx)
}

func (s *Service) DesiredForDevice(ctx context.Context, deviceID string) (*Release, error) {
	return s.repo.DesiredForDevice(ctx, deviceID)
}

func (s *Service) Assign(ctx context.Context, deviceID, version string) error {
	if err := ValidateVersion(version); err != nil {
		return err
	}
	ok, err := s.repo.DeviceExists(ctx, deviceID)
	if err != nil {
		return err
	}
	if !ok {
		return fmt.Errorf("device %s not found", deviceID)
	}
	rel, err := s.repo.GetByVersion(ctx, version)
	if err != nil {
		return err
	}
	return s.repo.Assign(ctx, deviceID, rel.ID)
}

func (s *Service) AssignAll(ctx context.Context, version string) (int64, error) {
	if err := ValidateVersion(version); err != nil {
		return 0, err
	}
	rel, err := s.repo.GetByVersion(ctx, version)
	if err != nil {
		return 0, err
	}
	return s.repo.AssignAll(ctx, rel.ID)
}

func (s *Service) Clear(ctx context.Context, deviceID string) error {
	ok, err := s.repo.DeviceExists(ctx, deviceID)
	if err != nil {
		return err
	}
	if !ok {
		return fmt.Errorf("device %s not found", deviceID)
	}
	return s.repo.Clear(ctx, deviceID)
}

func (s *Service) OpenBinary(ctx context.Context, deviceID string) (*Release, io.ReadCloser, error) {
	rel, err := s.repo.DesiredForDevice(ctx, deviceID)
	if err != nil {
		return nil, nil, err
	}
	if rel == nil {
		return nil, nil, nil
	}
	reader, _, err := s.storage.Get(ctx, rel.StorageKey)
	if err != nil {
		return nil, nil, err
	}
	return rel, reader, nil
}
