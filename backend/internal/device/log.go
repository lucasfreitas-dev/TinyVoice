package device

import (
	"fmt"
	"strings"
	"time"
	"unicode/utf8"
)

const (
	MaxLogEntriesPerRequest = 50
	MaxLogMessageRunes      = 256
	MaxLogsPerDevice        = 1000
)

type LogEntry struct {
	ID              string
	DeviceID        string
	TsMs            *int64
	Level           string
	Message         string
	FirmwareVersion string
	CreatedAt       time.Time
}

type LogIn struct {
	TsMs    *int64 `json:"ts_ms"`
	Level   string `json:"level"`
	Message string `json:"msg"`
}

func NormalizeLevel(level string) string {
	switch strings.ToLower(strings.TrimSpace(level)) {
	case "debug", "warn", "error":
		return strings.ToLower(strings.TrimSpace(level))
	default:
		return "info"
	}
}

func SanitizeLogMessage(msg string) string {
	msg = strings.TrimSpace(msg)
	if msg == "" {
		return ""
	}
	if utf8.RuneCountInString(msg) <= MaxLogMessageRunes {
		return msg
	}
	runes := []rune(msg)
	return string(runes[:MaxLogMessageRunes])
}

func NormalizeLogs(in []LogIn) ([]LogIn, error) {
	if len(in) > MaxLogEntriesPerRequest {
		return nil, fmt.Errorf("too many log entries (max %d)", MaxLogEntriesPerRequest)
	}
	out := make([]LogIn, 0, len(in))
	for _, e := range in {
		msg := SanitizeLogMessage(e.Message)
		if msg == "" {
			continue
		}
		out = append(out, LogIn{
			TsMs:    e.TsMs,
			Level:   NormalizeLevel(e.Level),
			Message: msg,
		})
	}
	return out, nil
}
