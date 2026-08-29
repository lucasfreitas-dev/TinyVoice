package evolution

import (
	"bytes"
	"context"
	"fmt"
	"io"
	"strings"

	"tinyvoice/backend/internal/messaging"
)

type Provider struct {
	client *Client
}

func NewProvider(baseURL, apiKey, instance string) *Provider {
	return &Provider{client: NewClient(baseURL, apiKey, instance)}
}

func (p *Provider) SendAudio(ctx context.Context, recipient string, audio messaging.AudioMessage) error {
	defer audio.Reader.Close()

	b64, err := EncodeBase64(audio.Reader)
	if err != nil {
		return fmt.Errorf("encode audio: %w", err)
	}

	fileName := audio.FileName
	if fileName == "" {
		fileName = "voice.opus"
	}

	return p.client.SendAudioBase64(ctx, recipient, b64, fileName)
}

func (p *Provider) DownloadMedia(ctx context.Context, messageRef string) (io.ReadCloser, int64, string, error) {
	parts := strings.SplitN(messageRef, "|", 2)
	id := parts[0]
	remoteJid := ""
	if len(parts) > 1 {
		remoteJid = parts[1]
	}

	data, mime, err := p.client.GetBase64FromMediaMessage(ctx, id, remoteJid)
	if err != nil {
		return nil, 0, "", err
	}
	return io.NopCloser(bytes.NewReader(data)), int64(len(data)), mime, nil
}
