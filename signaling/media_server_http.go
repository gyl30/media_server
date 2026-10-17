package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"mime"
	"net"
	"net/http"
	"strconv"
	"strings"
	"time"
)

type mediaServer struct {
	controlURL   string
	controlToken string
	mediaIP      string
	httpOrigin   string
	httpPort     uint16
	rtmpPort     uint16
	rtspPort     uint16
}

func (s mediaServer) publicHTTPOrigin() string {
	if s.httpOrigin != "" {
		return s.httpOrigin
	}
	return "http://" + net.JoinHostPort(s.mediaIP, strconv.Itoa(int(s.httpPort)))
}

type gb28181ReceiverRequest struct {
	streamID    string
	payloadType uint8
	ssrc        uint32
}

type receiverKind int

const (
	liveReceiver receiverKind = iota
	playbackReceiver
)

type pullCreateRequest struct {
	StreamID string  `json:"stream_id"`
	URL      string  `json:"url"`
	Username *string `json:"username,omitempty"`
	Password *string `json:"password,omitempty"`
	Token    *string `json:"token,omitempty"`
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

func (c *mediaServerHTTPClient) createReceiver(ctx context.Context, receiver gb28181ReceiverRequest, kind receiverKind) (uint16, error) {
	requestBody := struct {
		StreamID    string `json:"stream_id"`
		Transport   string `json:"transport"`
		PayloadType uint8  `json:"payload_type"`
		SSRC        uint32 `json:"ssrc"`
	}{
		StreamID: receiver.streamID, Transport: "udp", PayloadType: receiver.payloadType, SSRC: receiver.ssrc,
	}
	path := "/gb28181/receiver/create"
	if kind == playbackReceiver {
		path = "/gb28181/receiver/playback/create"
		requestBody.Transport = "tcp_passive"
	}
	response := struct {
		RTPPort uint16 `json:"rtp_port"`
	}{}
	if err := c.post(ctx, c.server.controlURL+path, requestBody, http.StatusCreated, &response); err != nil {
		return 0, err
	}
	if response.RTPPort == 0 || (kind == liveReceiver && response.RTPPort%2 != 0) {
		return 0, fmt.Errorf("invalid media server create response")
	}
	return response.RTPPort, nil
}

func (c *mediaServerHTTPClient) deleteReceiver(ctx context.Context, streamID string) error {
	requestBody := struct {
		StreamID string `json:"stream_id"`
	}{StreamID: streamID}
	return c.post(ctx, c.server.controlURL+"/receivers/delete", requestBody, http.StatusNoContent, nil)
}

type mediaReceiver struct {
	StreamID string `json:"stream_id"`
	Ready    bool   `json:"ready"`
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

func (c *mediaServerHTTPClient) updateReceiverSSRC(ctx context.Context, streamID string, ssrc uint32) error {
	requestBody := struct {
		StreamID string `json:"stream_id"`
		SSRC     uint32 `json:"ssrc"`
	}{StreamID: streamID, SSRC: ssrc}
	return c.post(ctx, c.server.controlURL+"/gb28181/receiver/update", requestBody, http.StatusNoContent, nil)
}

func (c *mediaServerHTTPClient) createPull(ctx context.Context, command pullCreateRequest) error {
	return c.post(ctx, c.server.controlURL+"/pull/create", command, http.StatusCreated, nil)
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
