package audio

import (
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
)

const (
	SampleRate    = 16000
	MinDurationMs = 500
	MaxDurationMs = 60000
)

type WAVInfo struct {
	DurationMs int
	SizeBytes  int64
}

func ValidateWAV(r io.Reader, maxBytes int64) (*WAVInfo, error) {
	const headerSize = 44
	header := make([]byte, headerSize)
	n, err := io.ReadFull(r, header)
	if err != nil {
		return nil, fmt.Errorf("read wav header: %w", err)
	}
	if n < headerSize || string(header[0:4]) != "RIFF" || string(header[8:12]) != "WAVE" {
		return nil, fmt.Errorf("invalid wav format")
	}

	dataSize := binary.LittleEndian.Uint32(header[40:44])
	byteRate := binary.LittleEndian.Uint32(header[28:32])
	if byteRate == 0 {
		return nil, fmt.Errorf("invalid wav byte rate")
	}

	durationMs := int(dataSize) * 1000 / int(byteRate)
	sizeBytes := int64(dataSize) + headerSize

	if durationMs < MinDurationMs {
		return nil, fmt.Errorf("recording too short: %dms (min %dms)", durationMs, MinDurationMs)
	}
	if durationMs > MaxDurationMs {
		return nil, fmt.Errorf("recording too long: %dms (max %dms)", durationMs, MaxDurationMs)
	}
	if sizeBytes > maxBytes {
		return nil, fmt.Errorf("file too large")
	}

	return &WAVInfo{DurationMs: durationMs, SizeBytes: sizeBytes}, nil
}

func ConvertToOpus(inputPath, outputPath string) error {
	cmd := exec.Command("ffmpeg", "-y", "-i", inputPath,
		"-c:a", "libopus", "-b:a", "32k", "-vbr", "on",
		"-ac", "1", "-ar", "16000", outputPath)
	cmd.Stderr = os.Stderr
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("ffmpeg convert to opus: %w", err)
	}
	return nil
}

func ConvertToWAV(inputPath, outputPath string) error {
	cmd := exec.Command("ffmpeg", "-y", "-i", inputPath,
		"-ac", "1", "-ar", "16000", "-c:a", "pcm_s16le", outputPath)
	cmd.Stderr = os.Stderr
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("ffmpeg convert to wav: %w", err)
	}
	return nil
}

func SaveTemp(prefix string, r io.Reader) (string, error) {
	f, err := os.CreateTemp("", prefix+"-*.wav")
	if err != nil {
		return "", err
	}
	defer f.Close()
	if _, err := io.Copy(f, r); err != nil {
		os.Remove(f.Name())
		return "", err
	}
	return f.Name(), nil
}

func TempOpusPath(wavPath string) string {
	return filepath.Join(filepath.Dir(wavPath), filepath.Base(wavPath)+".opus")
}
