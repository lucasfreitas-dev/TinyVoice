package storage

import (
	"context"
	"io"
)

type Provider interface {
	EnsureBucket(ctx context.Context) error
	Put(ctx context.Context, key string, reader io.Reader, size int64, contentType string) error
	Get(ctx context.Context, key string) (io.ReadCloser, int64, error)
	PutFile(ctx context.Context, key, localPath, contentType string) error
}
