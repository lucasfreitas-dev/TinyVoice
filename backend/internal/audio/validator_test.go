package audio

import (
	"bytes"
	"encoding/binary"
	"testing"
)

func buildWAVHeader(dataSize uint32, byteRate uint32) []byte {
	header := make([]byte, 44)
	copy(header[0:4], "RIFF")
	binary.LittleEndian.PutUint32(header[4:8], 36+dataSize)
	copy(header[8:12], "WAVE")
	copy(header[12:16], "fmt ")
	binary.LittleEndian.PutUint32(header[16:20], 16)
	binary.LittleEndian.PutUint16(header[20:22], 1)
	binary.LittleEndian.PutUint16(header[22:24], 1)
	binary.LittleEndian.PutUint32(header[24:28], 16000)
	binary.LittleEndian.PutUint32(header[28:32], byteRate)
	binary.LittleEndian.PutUint16(header[32:34], 2)
	binary.LittleEndian.PutUint16(header[34:36], 16)
	copy(header[36:40], "data")
	binary.LittleEndian.PutUint32(header[40:44], dataSize)
	return header
}

func appendWAVChunk(buf *bytes.Buffer, id string, payload []byte) {
	buf.WriteString(id)
	size := uint32(len(payload))
	_ = binary.Write(buf, binary.LittleEndian, size)
	buf.Write(payload)
	if size%2 == 1 {
		buf.WriteByte(0)
	}
}

func buildWAVHeaderWithLIST(dataSize, byteRate uint32) []byte {
	fmtPayload := make([]byte, 16)
	binary.LittleEndian.PutUint16(fmtPayload[0:2], 1)
	binary.LittleEndian.PutUint16(fmtPayload[2:4], 1)
	binary.LittleEndian.PutUint32(fmtPayload[4:8], 16000)
	binary.LittleEndian.PutUint32(fmtPayload[8:12], byteRate)
	binary.LittleEndian.PutUint16(fmtPayload[12:14], 2)
	binary.LittleEndian.PutUint16(fmtPayload[14:16], 16)

	listPayload := append([]byte("INFO"), "ISFT"...)
	listPayload = append(listPayload, 0x0e, 0x00, 0x00, 0x00)
	listPayload = append(listPayload, []byte("Lavf60.16.100")...)
	listPayload = append(listPayload, 0x00)

	dataPayload := make([]byte, dataSize)

	body := &bytes.Buffer{}
	appendWAVChunk(body, "fmt ", fmtPayload)
	appendWAVChunk(body, "LIST", listPayload)
	appendWAVChunk(body, "data", dataPayload)

	var out bytes.Buffer
	out.WriteString("RIFF")
	_ = binary.Write(&out, binary.LittleEndian, uint32(4+body.Len()))
	out.WriteString("WAVE")
	out.Write(body.Bytes())
	return out.Bytes()
}

func TestValidateWAVTooShort(t *testing.T) {
	byteRate := uint32(32000)
	dataSize := uint32(8000) // 250ms
	header := buildWAVHeader(dataSize, byteRate)
	_, err := ValidateWAV(bytes.NewReader(header), 1024*1024)
	if err == nil {
		t.Fatal("expected error for short recording")
	}
}

func TestValidateWAVValid(t *testing.T) {
	byteRate := uint32(32000)
	dataSize := uint32(32000) // 1 second
	header := buildWAVHeader(dataSize, byteRate)
	info, err := ValidateWAV(bytes.NewReader(header), 1024*1024)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if info.DurationMs != 1000 {
		t.Fatalf("expected 1000ms, got %d", info.DurationMs)
	}
}

func TestValidateWAVWithLISTChunk(t *testing.T) {
	byteRate := uint32(32000)
	dataSize := uint32(32000) // 1 second
	header := buildWAVHeaderWithLIST(dataSize, byteRate)
	info, err := ValidateWAV(bytes.NewReader(header), 1024*1024)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if info.DurationMs != 1000 {
		t.Fatalf("expected 1000ms, got %d", info.DurationMs)
	}
}

func TestValidateWAVInvalidFormat(t *testing.T) {
	_, err := ValidateWAV(bytes.NewReader([]byte("not a wav")), 1024)
	if err == nil {
		t.Fatal("expected error")
	}
}
