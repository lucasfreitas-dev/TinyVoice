package conversation

import "testing"

func TestParseChannelDefaultWhatsApp(t *testing.T) {
	got, err := ParseChannel("")
	if err != nil {
		t.Fatal(err)
	}
	if got != ChannelWhatsApp {
		t.Fatalf("got %q, want %s", got, ChannelWhatsApp)
	}
}

func TestParseChannelTelegram(t *testing.T) {
	got, err := ParseChannel(" Telegram ")
	if err != nil {
		t.Fatal(err)
	}
	if got != ChannelTelegram {
		t.Fatalf("got %q, want %s", got, ChannelTelegram)
	}
}

func TestParseChannelRejectsUnknown(t *testing.T) {
	if _, err := ParseChannel("signal"); err == nil {
		t.Fatal("expected error")
	}
}
