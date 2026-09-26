package firmware

import "testing"

func TestValidateVersion(t *testing.T) {
	if err := ValidateVersion("0.1.0"); err != nil {
		t.Fatal(err)
	}
	if err := ValidateVersion("1.2.3-rc1"); err != nil {
		t.Fatal(err)
	}
	if err := ValidateVersion(""); err == nil {
		t.Fatal("expected error for empty version")
	}
	if err := ValidateVersion("has space"); err == nil {
		t.Fatal("expected error for spaces")
	}
}

func TestShouldUpdate(t *testing.T) {
	if ShouldUpdate("0.1.0", "") {
		t.Fatal("no desired version should not update")
	}
	if ShouldUpdate("0.1.0", "0.1.0") {
		t.Fatal("matching versions should not update")
	}
	if !ShouldUpdate("0.1.0", "0.2.0") {
		t.Fatal("expected update when versions differ")
	}
	if !ShouldUpdate("", "0.2.0") {
		t.Fatal("expected update when current is unknown")
	}
}

func TestStorageKey(t *testing.T) {
	if got := StorageKey("0.2.0"); got != "firmware/0.2.0.bin" {
		t.Fatalf("got %q", got)
	}
}
