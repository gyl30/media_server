package main

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"mime"
	"net/http"
	"strings"
	"time"
)

type gb28181ReceiverRequest struct {
	streamID    string
	streamName  string
	payloadType uint8
	ssrc        uint32
}

type gb28181ReceiverEndpoint struct {
	streamID    string
	streamName  string
	address     string
	rtpPort     uint16
	rtcpPort    uint16
	payloadType uint8
	ssrc        uint32
}

type rtspPullCreateRequest struct {
	StreamID   string  `json:"stream_id"`
	SourceID   string  `json:"source_id"`
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

type mediaServerHTTPClient struct {
	client *http.Client
}

func newMediaServerHTTPClient(timeout time.Duration) *mediaServerHTTPClient {
	return &mediaServerHTTPClient{client: &http.Client{Timeout: timeout}}
}

func (c *mediaServerHTTPClient) createUDPReceiver(
	ctx context.Context,
	server mediaServerInstance,
	receiver gb28181ReceiverRequest,
) (gb28181ReceiverEndpoint, error) {
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
		RTPPort  uint16 `json:"rtp_port"`
		RTCPPort uint16 `json:"rtcp_port"`
	}{}
	if err := c.post(ctx, server.controlURL+"/gb28181/receiver/create", requestBody, http.StatusCreated, &responseBody); err != nil {
		return gb28181ReceiverEndpoint{}, err
	}
	if responseBody.RTPPort == 0 || responseBody.RTPPort%2 != 0 || responseBody.RTCPPort != responseBody.RTPPort+1 {
		return gb28181ReceiverEndpoint{}, fmt.Errorf("invalid media server create response")
	}
	return gb28181ReceiverEndpoint{
		streamID:    receiver.streamID,
		streamName:  receiver.streamName,
		address:     server.mediaIP,
		rtpPort:     responseBody.RTPPort,
		rtcpPort:    responseBody.RTCPPort,
		payloadType: receiver.payloadType,
		ssrc:        receiver.ssrc,
	}, nil
}

func (c *mediaServerHTTPClient) deleteReceiver(ctx context.Context, server mediaServerInstance, streamID, streamName string) error {
	requestBody := struct {
		StreamID   string `json:"stream_id"`
		StreamName string `json:"stream_name"`
	}{StreamID: streamID, StreamName: streamName}
	return c.post(ctx, server.controlURL+"/gb28181/receiver/delete", requestBody, http.StatusNoContent, nil)
}

func (c *mediaServerHTTPClient) createRTSPPull(ctx context.Context, server mediaServerInstance, command rtspPullCreateRequest) error {
	return c.post(ctx, server.controlURL+"/rtsp/pull/create", command, http.StatusCreated, nil)
}

func (c *mediaServerHTTPClient) deleteRTSPPull(ctx context.Context, server mediaServerInstance, streamID, streamName string) error {
	requestBody := struct {
		StreamID   string `json:"stream_id"`
		StreamName string `json:"stream_name"`
	}{StreamID: streamID, StreamName: streamName}
	return c.post(ctx, server.controlURL+"/rtsp/pull/delete", requestBody, http.StatusNoContent, nil)
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
