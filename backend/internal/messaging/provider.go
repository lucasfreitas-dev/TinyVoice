package messaging

import (
	"context"
	"io"
)

type AudioMessage struct {
	Reader      io.ReadCloser
	Size        int64
	MimeType    string
	DurationSec int
	FileName    string
}

type MessagingProvider interface {
	SendAudio(ctx context.Context, recipient string, audio AudioMessage) error
	DownloadMedia(ctx context.Context, messageID string) (io.ReadCloser, int64, string, error)
}
