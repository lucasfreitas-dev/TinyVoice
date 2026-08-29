package device

import "testing"

func TestGenerateTokenUnique(t *testing.T) {
	a, err := GenerateToken()
	if err != nil {
		t.Fatal(err)
	}
	b, err := GenerateToken()
	if err != nil {
		t.Fatal(err)
	}
	if a == b {
		t.Fatal("tokens should be unique")
	}
	if len(a) < 32 {
		t.Fatalf("token too short: %d", len(a))
	}
}

func TestHashAndVerifyToken(t *testing.T) {
	token, err := GenerateToken()
	if err != nil {
		t.Fatal(err)
	}
	hash, err := HashToken(token)
	if err != nil {
		t.Fatal(err)
	}
	if !VerifyToken(token, hash) {
		t.Fatal("expected token to verify")
	}
	if VerifyToken("wrong-token", hash) {
		t.Fatal("wrong token should not verify")
	}
}

func TestHashTokenDifferentHashes(t *testing.T) {
	token := "same-token-value"
	h1, _ := HashToken(token)
	h2, _ := HashToken(token)
	if h1 == h2 {
		t.Fatal("bcrypt hashes should differ due to salt")
	}
	if !VerifyToken(token, h1) || !VerifyToken(token, h2) {
		t.Fatal("both hashes should verify")
	}
}
