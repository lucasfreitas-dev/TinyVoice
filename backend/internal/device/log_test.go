package device

import "testing"

func TestNormalizeLevel(t *testing.T) {
	if got := NormalizeLevel("WARN"); got != "warn" {
		t.Fatalf("got %q", got)
	}
	if got := NormalizeLevel("nope"); got != "info" {
		t.Fatalf("got %q", got)
	}
}

func TestSanitizeLogMessage(t *testing.T) {
	if got := SanitizeLogMessage("  hi  "); got != "hi" {
		t.Fatalf("got %q", got)
	}
	long := stringsRepeat("x", MaxLogMessageRunes+10)
	if got := SanitizeLogMessage(long); len([]rune(got)) != MaxLogMessageRunes {
		t.Fatalf("expected truncation to %d, got %d", MaxLogMessageRunes, len([]rune(got)))
	}
}

func TestNormalizeLogsSkipsEmpty(t *testing.T) {
	out, err := NormalizeLogs([]LogIn{
		{Level: "info", Message: ""},
		{Level: "error", Message: "boom"},
	})
	if err != nil {
		t.Fatal(err)
	}
	if len(out) != 1 || out[0].Message != "boom" || out[0].Level != "error" {
		t.Fatalf("unexpected %#v", out)
	}
}

func TestNormalizeLogsRejectsTooMany(t *testing.T) {
	in := make([]LogIn, MaxLogEntriesPerRequest+1)
	for i := range in {
		in[i] = LogIn{Message: "x"}
	}
	if _, err := NormalizeLogs(in); err == nil {
		t.Fatal("expected error")
	}
}

func stringsRepeat(s string, n int) string {
	out := make([]byte, 0, len(s)*n)
	for i := 0; i < n; i++ {
		out = append(out, s...)
	}
	return string(out)
}
