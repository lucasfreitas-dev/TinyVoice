package conversation

import (
	"fmt"
	"strings"
)

const (
	ChannelWhatsApp = "whatsapp"
	ChannelTelegram = "telegram"
)

func ParseChannel(s string) (string, error) {
	c := strings.ToLower(strings.TrimSpace(s))
	if c == "" {
		return ChannelWhatsApp, nil
	}
	switch c {
	case ChannelWhatsApp, ChannelTelegram:
		return c, nil
	default:
		return "", fmt.Errorf("unsupported channel %q (use whatsapp or telegram)", s)
	}
}
