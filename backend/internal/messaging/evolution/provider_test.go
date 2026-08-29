package evolution

import (
	"strings"
	"testing"
)

func TestNewProvider(t *testing.T) {
	p := NewProvider("http://localhost:8080", "test-key", "instance")
	if p == nil || p.client == nil {
		t.Fatal("provider not initialized")
	}
}

func TestSplitMessageRef(t *testing.T) {
	parts := strings.SplitN("abc|5511@s.whatsapp.net", "|", 2)
	if parts[0] != "abc" || parts[1] != "5511@s.whatsapp.net" {
		t.Fatal("split failed")
	}
}

func TestEncodeBase64(t *testing.T) {
	b64, err := EncodeBase64(strings.NewReader("hello"))
	if err != nil {
		t.Fatal(err)
	}
	if b64 == "" {
		t.Fatal("empty base64")
	}
}
