package audio

import (
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
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
	byteRate, dataSize, riffSize, err := parseWAVHeader(r)
	if err != nil {
		return nil, err
	}
	if byteRate == 0 {
		return nil, fmt.Errorf("invalid wav byte rate")
	}
	if dataSize == 0 {
		return nil, fmt.Errorf("missing wav data chunk")
	}

	durationMs := int(dataSize) * 1000 / int(byteRate)
	sizeBytes := int64(riffSize) + 8 // RIFF header is 8 bytes + chunk payload size

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

func parseWAVHeader(r io.Reader) (byteRate, dataSize, riffSize uint32, err error) {
	var riff [12]byte
	if _, err = io.ReadFull(r, riff[:]); err != nil {
		return 0, 0, 0, fmt.Errorf("read wav header: %w", err)
	}
	if string(riff[0:4]) != "RIFF" || string(riff[8:12]) != "WAVE" {
		return 0, 0, 0, fmt.Errorf("invalid wav format")
	}
	riffSize = binary.LittleEndian.Uint32(riff[4:8])

	for {
		var chunkHeader [8]byte
		if _, err = io.ReadFull(r, chunkHeader[:]); err != nil {
			return 0, 0, 0, fmt.Errorf("read wav chunk: %w", err)
		}

		chunkID := string(chunkHeader[0:4])
		chunkSize := binary.LittleEndian.Uint32(chunkHeader[4:8])

		switch chunkID {
		case "fmt ":
			fmtChunk := make([]byte, chunkSize)
			if _, err = io.ReadFull(r, fmtChunk); err != nil {
				return 0, 0, 0, fmt.Errorf("read fmt chunk: %w", err)
			}
			if len(fmtChunk) >= 12 {
				byteRate = binary.LittleEndian.Uint32(fmtChunk[8:12])
			}
		case "data":
			dataSize = chunkSize
			return byteRate, dataSize, riffSize, nil
		default:
			if _, err = io.CopyN(io.Discard, r, int64(chunkSize)); err != nil {
				return 0, 0, 0, fmt.Errorf("skip chunk %q: %w", chunkID, err)
			}
		}

		if chunkSize%2 == 1 {
			if _, err = io.CopyN(io.Discard, r, 1); err != nil {
				return 0, 0, 0, fmt.Errorf("skip chunk padding: %w", err)
			}
		}
	}
}

func ConvertToOpus(inputPath, outputPath string) error {
	cmd := exec.Command("ffmpeg", "-y", "-t", maxDurationSeconds(), "-i", inputPath,
		"-c:a", "libopus", "-b:a", "64k", "-vbr", "on",
		"-ac", "1", "-ar", "16000", outputPath)
	cmd.Stderr = os.Stderr
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("ffmpeg convert to opus: %w", err)
	}
	return nil
}

func ConvertToWAV(inputPath, outputPath string) error {
	cmd := exec.Command("ffmpeg", "-y", "-t", maxDurationSeconds(), "-i", inputPath,
		"-ac", "1", "-ar", "16000", "-c:a", "pcm_s16le", outputPath)
	cmd.Stderr = os.Stderr
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("ffmpeg convert to wav: %w", err)
	}
	return nil
}

func maxDurationSeconds() string {
	return strconv.FormatFloat(float64(MaxDurationMs)/1000, 'f', 3, 64)
}

// TrimToMaxDuration rewrites path in place if the WAV is longer than MaxDurationMs.
// Shorter files are left untouched. Used for device uploads that skip ConvertToWAV.
func TrimToMaxDuration(path string) error {
	f, err := os.Open(path)
	if err != nil {
		return fmt.Errorf("open wav: %w", err)
	}
	byteRate, dataSize, _, err := parseWAVHeader(f)
	f.Close()
	if err != nil {
		return err
	}
	if byteRate == 0 {
		return fmt.Errorf("invalid wav byte rate")
	}
	durationMs := int(dataSize) * 1000 / int(byteRate)
	if durationMs <= MaxDurationMs {
		return nil
	}

	tmp := path + ".trim"
	if err := ConvertToWAV(path, tmp); err != nil {
		os.Remove(tmp)
		return err
	}
	if err := os.Rename(tmp, path); err != nil {
		os.Remove(tmp)
		return fmt.Errorf("replace trimmed wav: %w", err)
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
