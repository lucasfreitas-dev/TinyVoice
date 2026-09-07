package telegram

import (
	"bytes"
	"context"
	"fmt"
	"io"

	"tinyvoice/backend/internal/messaging"
)

type Provider struct {
	client *Client
}

func NewProvider(botToken string) *Provider {
	return &Provider{client: NewClient(botToken)}
}

func NewProviderWithClient(client *Client) *Provider {
	return &Provider{client: client}
}

func (p *Provider) SendAudio(ctx context.Context, recipient string, audio messaging.AudioMessage) error {
	defer audio.Reader.Close()

	fileName := audio.FileName
	if fileName == "" {
		fileName = "voice.ogg"
	}

	if err := p.client.SendVoice(ctx, recipient, audio.Reader, fileName, audio.DurationSec); err != nil {
		return fmt.Errorf("telegram send voice: %w", err)
	}
	return nil
}

func (p *Provider) DownloadMedia(ctx context.Context, fileID string) (io.ReadCloser, int64, string, error) {
	data, mime, err := p.client.DownloadFile(ctx, fileID)
	if err != nil {
		return nil, 0, "", err
	}
	if mime == "" {
		mime = "audio/ogg"
	}
	return io.NopCloser(bytes.NewReader(data)), int64(len(data)), mime, nil
}
