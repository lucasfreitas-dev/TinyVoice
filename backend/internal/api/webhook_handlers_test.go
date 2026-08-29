package api

import "testing"

const (
	testLIDJID    = "123456789012345@lid"
	testPhoneJID  = "5511000000001@s.whatsapp.net"
	testPhoneE164 = "5511000000001"
)

func TestResolveRecipientUsesRemoteJidAltForLID(t *testing.T) {
	got := resolveRecipient(testLIDJID, testPhoneJID, "")
	if got != testPhoneE164 {
		t.Fatalf("got %q, want %s", got, testPhoneE164)
	}
}

func TestResolveRecipientUsesSenderPnForLID(t *testing.T) {
	got := resolveRecipient(testLIDJID, "", testPhoneJID)
	if got != testPhoneE164 {
		t.Fatalf("got %q, want %s", got, testPhoneE164)
	}
}

func TestResolveRecipientUsesRemoteJidWhenNotLID(t *testing.T) {
	got := resolveRecipient(testPhoneJID, "", "")
	if got != testPhoneE164 {
		t.Fatalf("got %q, want %s", got, testPhoneE164)
	}
}
