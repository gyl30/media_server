package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"mime"
	"net/http"
	"strings"
	"time"
)

type mediaServer struct {
	controlURL   string
	controlToken string
	mediaIP      string
	httpPort     uint16
}

type gb28181ReceiverRequest struct {
	streamID    string
	streamName  string
	payloadType uint8
	ssrc        uint32
}

type rtspPullCreateRequest struct {
	StreamID   string  `json:"stream_id"`
	StreamName string  `json:"stream_name"`
	URL        string  `json:"url"`
	Username   *string `json:"username,omitempty"`
	Password   *string `json:"password,omitempty"`
}

type mediaServerHTTPRejection struct {
	status int
	code   string
}

func (e *mediaServerHTTPRejection) Error() string {
	return fmt.Sprintf("media server rejected request: status=%d code=%s", e.status, e.code)
}

func isMediaServerNotFound(err error) bool {
	var rejection *mediaServerHTTPRejection
	return errors.As(err, &rejection) && rejection.status == http.StatusNotFound
}

type mediaServerHTTPClient struct {
	server mediaServer
	client *http.Client
}

func newMediaServerHTTPClient(server mediaServer, timeout time.Duration) *mediaServerHTTPClient {
	return &mediaServerHTTPClient{server: server, client: &http.Client{Timeout: timeout}}
}

func (c *mediaServerHTTPClient) authorize(request *http.Request) {
	if c.server.controlToken != "" {
		request.Header.Set("Authorization", "Bearer "+c.server.controlToken)
	}
}

func (c *mediaServerHTTPClient) timeoutContext() (context.Context, context.CancelFunc) {
	return context.WithTimeout(context.Background(), c.client.Timeout)
}

func (c *mediaServerHTTPClient) createUDPReceiver(ctx context.Context, receiver gb28181ReceiverRequest) (uint16, error) {
	requestBody := struct {
		StreamID    string `json:"stream_id"`
		StreamName  string `json:"stream_name"`
		Transport   string `json:"transport"`
		PayloadType uint8  `json:"payload_type"`
		SSRC        uint32 `json:"ssrc"`
	}{
		StreamID: receiver.streamID, StreamName: receiver.streamName, Transport: "udp", PayloadType: receiver.payloadType, SSRC: receiver.ssrc,
	}
	responseBody := struct {
		RTPPort uint16 `json:"rtp_port"`
	}{}
	if err := c.post(ctx, c.server.controlURL+"/gb28181/receiver/create", requestBody, http.StatusCreated, &responseBody); err != nil {
		return 0, err
	}
	if responseBody.RTPPort == 0 || responseBody.RTPPort%2 != 0 {
		return 0, fmt.Errorf("invalid media server create response")
	}
	return responseBody.RTPPort, nil
}

func (c *mediaServerHTTPClient) deleteReceiver(ctx context.Context, streamID, streamName string) error {
	requestBody := struct {
		StreamID   string `json:"stream_id"`
		StreamName string `json:"stream_name"`
	}{StreamID: streamID, StreamName: streamName}
	return c.post(ctx, c.server.controlURL+"/gb28181/receiver/delete", requestBody, http.StatusNoContent, nil)
}

type mediaReceiver struct {
	StreamName string `json:"stream_name"`
	StreamID   string `json:"stream_id"`
}

func (c *mediaServerHTTPClient) listReceivers(ctx context.Context) ([]mediaReceiver, error) {
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, c.server.controlURL+"/receivers", nil)
	if err != nil {
		return nil, err
	}
	c.authorize(request)
	response, err := c.client.Do(request)
	if err != nil {
		return nil, fmt.Errorf("media server request failed: %w", err)
	}
	defer response.Body.Close()
	mediaType, _, err := mime.ParseMediaType(response.Header.Get("Content-Type"))
	if response.StatusCode != http.StatusOK || err != nil || !strings.EqualFold(mediaType, "application/json") {
		return nil, fmt.Errorf("invalid media server receiver list response: status=%d", response.StatusCode)
	}
	var body struct {
		Receivers []mediaReceiver `json:"receivers"`
	}
	if err := json.NewDecoder(io.LimitReader(response.Body, 16*1024*1024)).Decode(&body); err != nil {
		return nil, fmt.Errorf("invalid media server receiver list: %w", err)
	}
	return body.Receivers, nil
}

func (c *mediaServerHTTPClient) updateReceiverSSRC(ctx context.Context, streamID, streamName string, ssrc uint32) error {
	requestBody := struct {
		StreamID   string `json:"stream_id"`
		StreamName string `json:"stream_name"`
		SSRC       uint32 `json:"ssrc"`
	}{StreamID: streamID, StreamName: streamName, SSRC: ssrc}
	return c.post(ctx, c.server.controlURL+"/gb28181/receiver/update", requestBody, http.StatusNoContent, nil)
}

func (c *mediaServerHTTPClient) createRTSPPull(ctx context.Context, command rtspPullCreateRequest) error {
	return c.post(ctx, c.server.controlURL+"/rtsp/pull/create", command, http.StatusCreated, nil)
}

func (c *mediaServerHTTPClient) deleteRTSPPull(ctx context.Context, streamID, streamName string) error {
	requestBody := struct {
		StreamID   string `json:"stream_id"`
		StreamName string `json:"stream_name"`
	}{StreamID: streamID, StreamName: streamName}
	return c.post(ctx, c.server.controlURL+"/rtsp/pull/delete", requestBody, http.StatusNoContent, nil)
}

func (c *mediaServerHTTPClient) post(ctx context.Context, url string, requestBody any, successStatus int, responseBody any) error {
	body, err := json.Marshal(requestBody)
	if err != nil {
		return err
	}
	request, err := http.NewRequestWithContext(ctx, http.MethodPost, url, bytes.NewReader(body))
	if err != nil {
		return err
	}
	request.Header.Set("Content-Type", "application/json")
	c.authorize(request)
	response, err := c.client.Do(request)
	if err != nil {
		return fmt.Errorf("media server request failed: %w", err)
	}
	defer response.Body.Close()
	if response.StatusCode != successStatus {
		mediaType, _, err := mime.ParseMediaType(response.Header.Get("Content-Type"))
		if err != nil || !strings.EqualFold(mediaType, "application/json") {
			return fmt.Errorf("invalid media server response content type")
		}
		decoder := json.NewDecoder(io.LimitReader(response.Body, 64*1024))
		var failure struct {
			Code string `json:"error"`
		}
		if err := decoder.Decode(&failure); err != nil || failure.Code == "" {
			failure.Code = "invalid_response"
		}
		return &mediaServerHTTPRejection{status: response.StatusCode, code: failure.Code}
	}
	if responseBody == nil {
		return nil
	}
	mediaType, _, err := mime.ParseMediaType(response.Header.Get("Content-Type"))
	if err != nil || !strings.EqualFold(mediaType, "application/json") {
		return fmt.Errorf("invalid media server response content type")
	}
	decoder := json.NewDecoder(io.LimitReader(response.Body, 64*1024))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(responseBody); err != nil {
		return fmt.Errorf("invalid media server response: %w", err)
	}
	var extra any
	if decoder.Decode(&extra) != io.EOF {
		return fmt.Errorf("invalid media server response")
	}
	return nil
}
