package device

import "context"

type Service struct {
	repo *Repository
}

func NewService(repo *Repository) *Service {
	return &Service{repo: repo}
}

type CreateResult struct {
	Device *Device
	Token  string
}

func (s *Service) Create(ctx context.Context, name string) (*CreateResult, error) {
	token, err := GenerateToken()
	if err != nil {
		return nil, err
	}
	hash, err := HashToken(token)
	if err != nil {
		return nil, err
	}
	d, err := s.repo.Create(ctx, name, hash)
	if err != nil {
		return nil, err
	}
	return &CreateResult{Device: d, Token: token}, nil
}

func (s *Service) List(ctx context.Context) ([]Device, error) {
	return s.repo.List(ctx)
}

func (s *Service) BindConversation(ctx context.Context, deviceID, conversationID string) error {
	return s.repo.BindConversation(ctx, deviceID, conversationID)
}

func (s *Service) Authenticate(ctx context.Context, token string) (*Device, error) {
	return s.repo.Authenticate(ctx, token)
}

func (s *Service) UpdateLastSeen(ctx context.Context, id string) error {
	return s.repo.UpdateLastSeen(ctx, id)
}

func (s *Service) GetConversationID(ctx context.Context, deviceID string) (string, error) {
	return s.repo.GetConversationID(ctx, deviceID)
}
