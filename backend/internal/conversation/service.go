package conversation

import "context"

type Service struct {
	repo *Repository
}

func NewService(repo *Repository) *Service {
	return &Service{repo: repo}
}

func (s *Service) Create(ctx context.Context, name, recipient string) (*Conversation, error) {
	return s.repo.Create(ctx, name, recipient)
}

func (s *Service) GetByID(ctx context.Context, id string) (*Conversation, error) {
	return s.repo.GetByID(ctx, id)
}

func (s *Service) List(ctx context.Context) ([]Conversation, error) {
	return s.repo.List(ctx)
}
