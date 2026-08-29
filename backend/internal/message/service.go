package message

import (
	"context"
	"fmt"
	"io"
	"time"

	"github.com/google/uuid"

	"tinyvoice/backend/internal/conversation"
	"tinyvoice/backend/internal/device"
	"tinyvoice/backend/internal/storage"
)

type Service struct {
	repo       *Repository
	deviceRepo *device.Repository
	convRepo   *conversation.Repository
	storage    storage.Provider
}

func NewService(
	repo *Repository,
	deviceRepo *device.Repository,
	convRepo *conversation.Repository,
	storage storage.Provider,
) *Service {
	return &Service{
		repo:       repo,
		deviceRepo: deviceRepo,
		convRepo:   convRepo,
		storage:    storage,
	}
}

func (s *Service) CreateOutbound(ctx context.Context, deviceID string, reader io.Reader, size int64, mimeType string, durationMs int) (*Message, error) {
	convID, err := s.deviceRepo.GetConversationID(ctx, deviceID)
	if err != nil {
		return nil, fmt.Errorf("device not bound to conversation: %w", err)
	}

	key := fmt.Sprintf("audio/outbound/%s", uuid.New().String())
	if err := s.storage.Put(ctx, key, reader, size, mimeType); err != nil {
		return nil, fmt.Errorf("store audio: %w", err)
	}

	dur := durationMs
	sizeBytes := size
	msg := &Message{
		ConversationID:  convID,
		DeviceID:        deviceID,
		Direction:       DirectionOutbound,
		Status:          StatusPending,
		AudioStorageKey: &key,
		AudioDurationMs: &dur,
		AudioSizeBytes:  &sizeBytes,
		MimeType:        &mimeType,
	}
	return s.repo.Create(ctx, msg)
}

func (s *Service) GetNextAvailable(ctx context.Context, deviceID string) (*Message, error) {
	return s.repo.GetNextAvailable(ctx, deviceID)
}

func (s *Service) GetByID(ctx context.Context, id string) (*Message, error) {
	return s.repo.GetByID(ctx, id)
}

func (s *Service) MarkPlayed(ctx context.Context, id, deviceID string) error {
	return s.repo.MarkPlayed(ctx, id, deviceID)
}

func (s *Service) OpenAudio(ctx context.Context, id, deviceID string) (io.ReadCloser, string, int64, error) {
	msg, err := s.repo.GetByID(ctx, id)
	if err != nil {
		return nil, "", 0, err
	}
	if msg.DeviceID != deviceID {
		return nil, "", 0, fmt.Errorf("message not owned by device")
	}
	if msg.Status != StatusAvailable {
		return nil, "", 0, fmt.Errorf("message not available")
	}
	if msg.AudioStorageKey == nil {
		return nil, "", 0, fmt.Errorf("no audio stored")
	}
	rc, size, err := s.storage.Get(ctx, *msg.AudioStorageKey)
	if err != nil {
		return nil, "", 0, err
	}
	mime := "audio/wav"
	if msg.MimeType != nil {
		mime = *msg.MimeType
	}
	return rc, mime, size, nil
}

func (s *Service) CreateInbound(ctx context.Context, deviceID, convID, externalID, key, mimeType string, durationMs int, sizeBytes int64) (*Message, error) {
	exists, err := s.repo.ExistsByExternalID(ctx, externalID)
	if err != nil {
		return nil, err
	}
	if exists {
		return nil, fmt.Errorf("duplicate message")
	}

	dur := durationMs
	size := sizeBytes
	now := time.Now().UTC()
	msg := &Message{
		ConversationID:  convID,
		DeviceID:        deviceID,
		Direction:       DirectionInbound,
		Status:          StatusAvailable,
		AudioStorageKey: &key,
		AudioDurationMs: &dur,
		AudioSizeBytes:  &size,
		MimeType:        &mimeType,
		ExternalID:      &externalID,
		ReceivedAt:      &now,
	}
	return s.repo.Create(ctx, msg)
}

func (s *Service) ClaimPending(ctx context.Context) (*Message, error) {
	return s.repo.ClaimPending(ctx)
}

func (s *Service) MarkSent(ctx context.Context, id string) error {
	return s.repo.MarkSent(ctx, id)
}

func (s *Service) MarkFailed(ctx context.Context, id, errMsg string) error {
	return s.repo.MarkFailed(ctx, id, errMsg)
}

func (s *Service) RequeuePending(ctx context.Context, id, errMsg string) error {
	return s.repo.RequeuePending(ctx, id, errMsg)
}

func (s *Service) RecordWebhookEvent(ctx context.Context, provider, externalID string) (bool, error) {
	return s.repo.RecordWebhookEvent(ctx, provider, externalID)
}

func (s *Service) GetConversation(ctx context.Context, convID string) (*conversation.Conversation, error) {
	return s.convRepo.GetByID(ctx, convID)
}

func (s *Service) GetStorageKey(ctx context.Context, id string) (string, error) {
	msg, err := s.repo.GetByID(ctx, id)
	if err != nil {
		return "", err
	}
	if msg.AudioStorageKey == nil {
		return "", fmt.Errorf("no storage key")
	}
	return *msg.AudioStorageKey, nil
}

func (s *Service) OpenStorage(ctx context.Context, key string) (io.ReadCloser, int64, error) {
	return s.storage.Get(ctx, key)
}
