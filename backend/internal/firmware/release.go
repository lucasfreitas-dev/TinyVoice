package firmware

import (
	"fmt"
	"regexp"
	"strings"
	"time"
	"unicode/utf8"
)

// MaxBinaryBytes is the ESP32 default OTA app slot (1.25 MiB).
const MaxBinaryBytes = 0x140000

var versionRe = regexp.MustCompile(`^[A-Za-z0-9._+-]{1,32}$`)

type Release struct {
	ID         string
	Version    string
	StorageKey string
	SHA256     string
	SizeBytes  int64
	CreatedAt  time.Time
}

func ValidateVersion(version string) error {
	v := strings.TrimSpace(version)
	if v == "" {
		return fmt.Errorf("version is required")
	}
	if utf8.RuneCountInString(v) > 32 || !versionRe.MatchString(v) {
		return fmt.Errorf("invalid version %q", version)
	}
	return nil
}

func StorageKey(version string) string {
	return "firmware/" + version + ".bin"
}

func ShouldUpdate(current, desired string) bool {
	current = strings.TrimSpace(current)
	desired = strings.TrimSpace(desired)
	if desired == "" {
		return false
	}
	return current != desired
}
