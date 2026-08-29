package middleware

import (
	"context"
	"net/http"
	"net/http/httptest"
	"testing"

	"tinyvoice/backend/internal/device"
)

type deviceAuthenticator interface {
	Authenticate(ctx context.Context, token string) (*device.Device, error)
}

type authStub struct {
	valid string
}

func (a *authStub) Authenticate(_ context.Context, token string) (*device.Device, error) {
	if token == a.valid {
		return &device.Device{ID: "dev-1", Name: "test", Enabled: true}, nil
	}
	return nil, errInvalid{}
}

type errInvalid struct{}

func (errInvalid) Error() string { return "invalid token" }

func TestDeviceAuthMissingHeader(t *testing.T) {
	handler := deviceAuthWith(&authStub{valid: "secret-token"})(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
	}))

	req := httptest.NewRequest(http.MethodGet, "/", nil)
	rec := httptest.NewRecorder()
	handler.ServeHTTP(rec, req)

	if rec.Code != http.StatusUnauthorized {
		t.Fatalf("expected 401, got %d", rec.Code)
	}
}

func TestDeviceAuthValidToken(t *testing.T) {
	handler := deviceAuthWith(&authStub{valid: "secret-token"})(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		d := DeviceFromContext(r.Context())
		if d == nil || d.ID != "dev-1" {
			t.Fatal("device not in context")
		}
		w.WriteHeader(http.StatusOK)
	}))

	req := httptest.NewRequest(http.MethodGet, "/", nil)
	req.Header.Set("Authorization", "Bearer secret-token")
	rec := httptest.NewRecorder()
	handler.ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d", rec.Code)
	}
}

func deviceAuthWith(auth deviceAuthenticator) func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			authHeader := r.Header.Get("Authorization")
			if len(authHeader) < 8 || authHeader[:7] != "Bearer " {
				http.Error(w, `{"error":"unauthorized"}`, http.StatusUnauthorized)
				return
			}
			token := authHeader[7:]
			d, err := auth.Authenticate(r.Context(), token)
			if err != nil {
				http.Error(w, `{"error":"unauthorized"}`, http.StatusUnauthorized)
				return
			}
			ctx := context.WithValue(r.Context(), DeviceContextKey, d)
			next.ServeHTTP(w, r.WithContext(ctx))
		})
	}
}
