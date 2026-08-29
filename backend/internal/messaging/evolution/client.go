package evolution

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"
	"time"
)

type Client struct {
	baseURL    string
	apiKey     string
	instance   string
	httpClient *http.Client
}

func NewClient(baseURL, apiKey, instance string) *Client {
	return &Client{
		baseURL:    strings.TrimRight(baseURL, "/"),
		apiKey:     apiKey,
		instance:   instance,
		httpClient: &http.Client{Timeout: 60 * time.Second},
	}
}

type sendAudioRequest struct {
	Number       string `json:"number"`
	Audio        string `json:"audio"`
	Encoding     bool   `json:"encoding,omitempty"`
	FileName     string `json:"fileName,omitempty"`
}

func (c *Client) SendAudioBase64(ctx context.Context, recipient, b64, fileName string) error {
	body, _ := json.Marshal(sendAudioRequest{
		Number:   recipient,
		Audio:    b64,
		Encoding: true,
		FileName: fileName,
	})

	url := fmt.Sprintf("%s/message/sendWhatsAppAudio/%s", c.baseURL, c.instance)
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, url, bytes.NewReader(body))
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("apikey", c.apiKey)

	resp, err := c.httpClient.Do(req)
	if err != nil {
		return fmt.Errorf("send audio request: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode >= 300 {
		b, _ := io.ReadAll(resp.Body)
		return fmt.Errorf("send audio failed: status=%d body=%s", resp.StatusCode, string(b))
	}
	return nil
}

type base64MediaRequest struct {
	Message struct {
		Key struct {
			ID        string `json:"id"`
			RemoteJid string `json:"remoteJid"`
		} `json:"key"`
	} `json:"message"`
	ConvertToMp4 bool `json:"convertToMp4"`
}

func (c *Client) GetBase64FromMediaMessage(ctx context.Context, messageID, remoteJid string) ([]byte, string, error) {
	payload := base64MediaRequest{}
	payload.Message.Key.ID = messageID
	payload.Message.Key.RemoteJid = remoteJid
	payload.ConvertToMp4 = false

	body, _ := json.Marshal(payload)
	url := fmt.Sprintf("%s/chat/getBase64FromMediaMessage/%s", c.baseURL, c.instance)
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, url, bytes.NewReader(body))
	if err != nil {
		return nil, "", err
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("apikey", c.apiKey)

	resp, err := c.httpClient.Do(req)
	if err != nil {
		return nil, "", fmt.Errorf("get base64 media: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode >= 300 {
		b, _ := io.ReadAll(resp.Body)
		return nil, "", fmt.Errorf("get base64 failed: status=%d body=%s", resp.StatusCode, string(b))
	}

	var result struct {
		Base64   string `json:"base64"`
		MimeType string `json:"mimetype"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&result); err != nil {
		return nil, "", fmt.Errorf("decode response: %w", err)
	}

	data, err := base64.StdEncoding.DecodeString(result.Base64)
	if err != nil {
		return nil, "", fmt.Errorf("decode base64: %w", err)
	}
	return data, result.MimeType, nil
}

func EncodeBase64(r io.Reader) (string, error) {
	data, err := io.ReadAll(r)
	if err != nil {
		return "", err
	}
	return base64.StdEncoding.EncodeToString(data), nil
}
