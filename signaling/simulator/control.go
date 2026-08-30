package main

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"
	"time"
)

type liveControlRequest struct {
	DeviceID  string `json:"device_id"`
	ChannelID string `json:"channel_id"`
}

type liveStartResponse struct {
	Result     string `json:"result"`
	StreamName string `json:"stream_name"`
	State      string `json:"state"`
	SSRC       uint32 `json:"ssrc"`
	RTPPort    uint16 `json:"rtp_port"`
}

type controlClient struct {
	baseURL string
	client  *http.Client
}

func newControlClient(baseURL string) controlClient {
	return controlClient{
		baseURL: strings.TrimSuffix(baseURL, "/"),
		client:  &http.Client{Timeout: 15 * time.Second},
	}
}

func (c controlClient) startLive(ctx context.Context, deviceID, channelID string) (liveStartResponse, error) {
	var result liveStartResponse
	if err := c.post(ctx, "/internal/live/start", liveControlRequest{DeviceID: deviceID, ChannelID: channelID}, http.StatusCreated, &result); err != nil {
		return liveStartResponse{}, err
	}
	return result, nil
}

func (c controlClient) stopLive(ctx context.Context, deviceID, channelID string) error {
	return c.post(ctx, "/internal/live/stop", liveControlRequest{DeviceID: deviceID, ChannelID: channelID}, http.StatusOK, nil)
}

func (c controlClient) post(ctx context.Context, path string, value any, expected int, output any) error {
	body, err := json.Marshal(value)
	if err != nil {
		return err
	}
	request, err := http.NewRequestWithContext(ctx, http.MethodPost, c.baseURL+path, bytes.NewReader(body))
	if err != nil {
		return err
	}
	request.Header.Set("Content-Type", "application/json")
	response, err := c.client.Do(request)
	if err != nil {
		return err
	}
	defer response.Body.Close()
	if response.StatusCode != expected {
		data, _ := io.ReadAll(io.LimitReader(response.Body, 4096))
		return fmt.Errorf("%s returned HTTP %d: %s", path, response.StatusCode, strings.TrimSpace(string(data)))
	}
	if output == nil {
		if _, err := io.Copy(io.Discard, response.Body); err != nil {
			return fmt.Errorf("read %s response: %w", path, err)
		}
		return nil
	}
	if err := json.NewDecoder(response.Body).Decode(output); err != nil {
		return fmt.Errorf("decode %s response: %w", path, err)
	}
	return nil
}
