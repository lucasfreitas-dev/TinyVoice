package telegram

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"mime/multipart"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"time"
)

const defaultAPIBase = "https://api.telegram.org"

type Client struct {
	botToken   string
	apiBase    string
	httpClient *http.Client
}

func NewClient(botToken string) *Client {
	return NewClientWithBase(botToken, defaultAPIBase)
}

func NewClientWithBase(botToken, apiBase string) *Client {
	if apiBase == "" {
		apiBase = defaultAPIBase
	}
	return &Client{
		botToken:   botToken,
		apiBase:    strings.TrimRight(apiBase, "/"),
		httpClient: &http.Client{Timeout: 60 * time.Second},
	}
}

type apiResponse struct {
	OK          bool            `json:"ok"`
	Description string          `json:"description"`
	Result      json.RawMessage `json:"result"`
}

type File struct {
	FileID   string `json:"file_id"`
	FilePath string `json:"file_path"`
	FileSize int64  `json:"file_size"`
}

func (c *Client) methodURL(method string) string {
	return fmt.Sprintf("%s/bot%s/%s", c.apiBase, c.botToken, method)
}

func (c *Client) fileURL(filePath string) string {
	return fmt.Sprintf("%s/file/bot%s/%s", c.apiBase, c.botToken, filePath)
}

func (c *Client) SendVoice(ctx context.Context, chatID string, audio io.Reader, fileName string, durationSec int) error {
	if fileName == "" {
		fileName = "voice.ogg"
	}

	var body bytes.Buffer
	w := multipart.NewWriter(&body)
	if err := w.WriteField("chat_id", chatID); err != nil {
		return err
	}
	if durationSec > 0 {
		if err := w.WriteField("duration", strconv.Itoa(durationSec)); err != nil {
			return err
		}
	}
	part, err := w.CreateFormFile("voice", fileName)
	if err != nil {
		return err
	}
	if _, err := io.Copy(part, audio); err != nil {
		return fmt.Errorf("write voice part: %w", err)
	}
	if err := w.Close(); err != nil {
		return err
	}

	req, err := http.NewRequestWithContext(ctx, http.MethodPost, c.methodURL("sendVoice"), &body)
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", w.FormDataContentType())

	resp, err := c.httpClient.Do(req)
	if err != nil {
		return fmt.Errorf("send voice request: %w", err)
	}
	defer resp.Body.Close()

	var result apiResponse
	if err := json.NewDecoder(resp.Body).Decode(&result); err != nil {
		return fmt.Errorf("decode sendVoice: %w", err)
	}
	if resp.StatusCode >= 300 || !result.OK {
		return fmt.Errorf("sendVoice failed: status=%d desc=%s", resp.StatusCode, result.Description)
	}
	return nil
}

func (c *Client) GetFile(ctx context.Context, fileID string) (*File, error) {
	u, err := url.Parse(c.methodURL("getFile"))
	if err != nil {
		return nil, err
	}
	q := u.Query()
	q.Set("file_id", fileID)
	u.RawQuery = q.Encode()

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, u.String(), nil)
	if err != nil {
		return nil, err
	}

	resp, err := c.httpClient.Do(req)
	if err != nil {
		return nil, fmt.Errorf("getFile request: %w", err)
	}
	defer resp.Body.Close()

	var result apiResponse
	if err := json.NewDecoder(resp.Body).Decode(&result); err != nil {
		return nil, fmt.Errorf("decode getFile: %w", err)
	}
	if resp.StatusCode >= 300 || !result.OK {
		return nil, fmt.Errorf("getFile failed: status=%d desc=%s", resp.StatusCode, result.Description)
	}

	var file File
	if err := json.Unmarshal(result.Result, &file); err != nil {
		return nil, fmt.Errorf("decode getFile result: %w", err)
	}
	if file.FilePath == "" {
		return nil, fmt.Errorf("getFile missing file_path")
	}
	return &file, nil
}

func (c *Client) DownloadFile(ctx context.Context, fileID string) ([]byte, string, error) {
	file, err := c.GetFile(ctx, fileID)
	if err != nil {
		return nil, "", err
	}

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, c.fileURL(file.FilePath), nil)
	if err != nil {
		return nil, "", err
	}

	resp, err := c.httpClient.Do(req)
	if err != nil {
		return nil, "", fmt.Errorf("download file: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode >= 300 {
		b, _ := io.ReadAll(resp.Body)
		return nil, "", fmt.Errorf("download file failed: status=%d body=%s", resp.StatusCode, string(b))
	}

	data, err := io.ReadAll(resp.Body)
	if err != nil {
		return nil, "", fmt.Errorf("read file: %w", err)
	}
	return data, resp.Header.Get("Content-Type"), nil
}

func (c *Client) SetWebhook(ctx context.Context, webhookURL, secret string) error {
	payload := map[string]any{
		"url":             webhookURL,
		"allowed_updates": []string{"message"},
	}
	if secret != "" {
		payload["secret_token"] = secret
	}
	body, err := json.Marshal(payload)
	if err != nil {
		return err
	}

	req, err := http.NewRequestWithContext(ctx, http.MethodPost, c.methodURL("setWebhook"), bytes.NewReader(body))
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", "application/json")

	resp, err := c.httpClient.Do(req)
	if err != nil {
		return fmt.Errorf("setWebhook request: %w", err)
	}
	defer resp.Body.Close()

	var result apiResponse
	if err := json.NewDecoder(resp.Body).Decode(&result); err != nil {
		return fmt.Errorf("decode setWebhook: %w", err)
	}
	if resp.StatusCode >= 300 || !result.OK {
		return fmt.Errorf("setWebhook failed: status=%d desc=%s", resp.StatusCode, result.Description)
	}
	return nil
}
