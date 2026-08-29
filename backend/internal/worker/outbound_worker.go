package worker

import (
	"context"
	"io"
	"log/slog"
	"os"
	"time"

	"tinyvoice/backend/internal/audio"
	"tinyvoice/backend/internal/message"
	"tinyvoice/backend/internal/messaging"
	"tinyvoice/backend/internal/storage"
)

type OutboundWorker struct {
	messages    *message.Service
	storage     storage.Provider
	provider    messaging.MessagingProvider
	maxAttempts int
	interval    time.Duration
	logger      *slog.Logger
}

func NewOutboundWorker(
	messages *message.Service,
	storage storage.Provider,
	provider messaging.MessagingProvider,
	maxAttempts int,
	interval time.Duration,
	logger *slog.Logger,
) *OutboundWorker {
	return &OutboundWorker{
		messages:    messages,
		storage:     storage,
		provider:    provider,
		maxAttempts: maxAttempts,
		interval:    interval,
		logger:      logger,
	}
}

func (w *OutboundWorker) Run(ctx context.Context) {
	ticker := time.NewTicker(w.interval)
	defer ticker.Stop()

	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			w.processOne(ctx)
		}
	}
}

func (w *OutboundWorker) processOne(ctx context.Context) {
	msg, err := w.messages.ClaimPending(ctx)
	if err != nil {
		w.logger.Error("worker_claim_failed", slog.String("error", err.Error()))
		return
	}
	if msg == nil {
		return
	}

	if err := w.send(ctx, msg); err != nil {
		w.logger.Error("message_send_failed",
			slog.String("message_id", msg.ID),
			slog.String("error", err.Error()),
		)
		if msg.Attempts >= w.maxAttempts {
			_ = w.messages.MarkFailed(ctx, msg.ID, err.Error())
			w.logger.Error("message_failed", slog.String("message_id", msg.ID))
		} else {
			backoff := time.Duration(msg.Attempts) * 5 * time.Second
			time.Sleep(backoff)
			_ = w.messages.RequeuePending(ctx, msg.ID, err.Error())
		}
		return
	}

	if err := w.messages.MarkSent(ctx, msg.ID); err != nil {
		w.logger.Error("message_mark_sent_failed", slog.String("message_id", msg.ID))
		return
	}
	w.logger.Info("message_sent", slog.String("message_id", msg.ID))
}

func (w *OutboundWorker) send(ctx context.Context, msg *message.Message) error {
	conv, err := w.messages.GetConversation(ctx, msg.ConversationID)
	if err != nil {
		return err
	}

	key, err := w.messages.GetStorageKey(ctx, msg.ID)
	if err != nil {
		return err
	}

	rc, _, err := w.storage.Get(ctx, key)
	if err != nil {
		return err
	}
	defer rc.Close()

	tmpWav, err := os.CreateTemp("", "outbound-*.wav")
	if err != nil {
		return err
	}
	wavPath := tmpWav.Name()
	defer os.Remove(wavPath)
	defer tmpWav.Close()

	if _, err := io.Copy(tmpWav, rc); err != nil {
		return err
	}
	tmpWav.Close()

	opusPath := audio.TempOpusPath(wavPath)
	defer os.Remove(opusPath)

	if err := audio.ConvertToOpus(wavPath, opusPath); err != nil {
		return err
	}

	opusFile, err := os.Open(opusPath)
	if err != nil {
		return err
	}
	defer opusFile.Close()

	opusInfo, err := opusFile.Stat()
	if err != nil {
		return err
	}

	durationSec := 0
	if msg.AudioDurationMs != nil {
		durationSec = *msg.AudioDurationMs / 1000
	}

	return w.provider.SendAudio(ctx, conv.WhatsAppRecipient, messaging.AudioMessage{
		Reader:      opusFile,
		Size:        opusInfo.Size(),
		MimeType:    "audio/ogg",
		DurationSec: durationSec,
		FileName:    "voice.opus",
	})
}
