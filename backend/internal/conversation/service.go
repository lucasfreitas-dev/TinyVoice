package conversation

import (
	"context"
	"fmt"
	"strings"
)

type Service struct {
	repo *Repository
}

func NewService(repo *Repository) *Service {
	return &Service{repo: repo}
}

func (s *Service) Create(ctx context.Context, name, channel, recipient string) (*Conversation, error) {
	ch, err := ParseChannel(channel)
	if err != nil {
		return nil, err
	}
	recipient = strings.TrimSpace(recipient)
	if recipient == "" {
		return nil, fmt.Errorf("recipient is required")
	}
	return s.repo.Create(ctx, name, ch, recipient)
}

func (s *Service) GetByID(ctx context.Context, id string) (*Conversation, error) {
	return s.repo.GetByID(ctx, id)
}

func (s *Service) List(ctx context.Context) ([]Conversation, error) {
	return s.repo.List(ctx)
}
