package api

import (
	"bytes"
	"net/http"
	"testing"
)

func TestDecodeHeartbeatEmptyBody(t *testing.T) {
	req := httptestRequest(http.MethodPost, "/", nil)
	got, err := decodeHeartbeat(req)
	if err != nil {
		t.Fatal(err)
	}
	if got.FirmwareVersion != "" || len(got.Logs) != 0 {
		t.Fatalf("unexpected %#v", got)
	}
}

func TestDecodeHeartbeatWithLogs(t *testing.T) {
	body := []byte(`{"firmware_version":"0.1.0","uptime_ms":10,"logs":[{"level":"info","msg":"boot"}]}`)
	req := httptestRequest(http.MethodPost, "/", body)
	got, err := decodeHeartbeat(req)
	if err != nil {
		t.Fatal(err)
	}
	if got.FirmwareVersion != "0.1.0" || got.UptimeMs == nil || *got.UptimeMs != 10 {
		t.Fatalf("unexpected %#v", got)
	}
	if len(got.Logs) != 1 || got.Logs[0].Message != "boot" {
		t.Fatalf("unexpected logs %#v", got.Logs)
	}
}

func httptestRequest(method, path string, body []byte) *http.Request {
	var r *http.Request
	if body == nil {
		r, _ = http.NewRequest(method, path, http.NoBody)
	} else {
		r, _ = http.NewRequest(method, path, bytes.NewReader(body))
	}
	return r
}
