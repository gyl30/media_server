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

type mediaInputRequest struct {
	streamName  string
	payloadType uint8
	ssrc        uint32
}

type mediaEndpoint struct {
	streamName  string
	address     string
	rtpPort     uint16
	rtcpPort    uint16
	payloadType uint8
	ssrc        uint32
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

func (c *mediaServerHTTPClient) createUDPInput(ctx context.Context, server mediaServerInstance, input mediaInputRequest) (mediaEndpoint, error) {
	requestBody := struct {
		StreamName  string `json:"stream_name"`
		Transport   string `json:"transport"`
		Address     string `json:"address"`
		PayloadType uint8  `json:"payload_type"`
		SSRC        uint32 `json:"ssrc"`
	}{
		StreamName: input.streamName, Transport: "udp", Address: server.mediaIP, PayloadType: input.payloadType, SSRC: input.ssrc,
	}
	responseBody := struct {
		Result   string `json:"result"`
		RTPPort  uint16 `json:"rtp_port"`
		RTCPPort uint16 `json:"rtcp_port"`
	}{}
	if err := c.post(ctx, server.controlURL+"/gb28181/create", requestBody, http.StatusCreated, &responseBody); err != nil {
		return mediaEndpoint{}, err
	}
	if responseBody.Result != "ok" || responseBody.RTPPort == 0 || responseBody.RTPPort%2 != 0 || responseBody.RTCPPort != responseBody.RTPPort+1 {
		return mediaEndpoint{}, fmt.Errorf("invalid media server create response")
	}
	return mediaEndpoint{
		streamName:  input.streamName,
		address:     server.mediaIP,
		rtpPort:     responseBody.RTPPort,
		rtcpPort:    responseBody.RTCPPort,
		payloadType: input.payloadType,
		ssrc:        input.ssrc,
	}, nil
}

func (c *mediaServerHTTPClient) deleteInput(ctx context.Context, server mediaServerInstance, streamName string) error {
	requestBody := struct {
		StreamName string `json:"stream_name"`
	}{StreamName: streamName}
	responseBody := struct {
		Result string `json:"result"`
	}{}
	if err := c.post(ctx, server.controlURL+"/gb28181/delete", requestBody, http.StatusOK, &responseBody); err != nil {
		return err
	}
	if responseBody.Result != "ok" {
		return fmt.Errorf("invalid media server delete response")
	}
	return nil
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
	mediaType, _, err := mime.ParseMediaType(response.Header.Get("Content-Type"))
	if err != nil || !strings.EqualFold(mediaType, "application/json") {
		return fmt.Errorf("invalid media server response content type")
	}
	decoder := json.NewDecoder(io.LimitReader(response.Body, 64*1024))
	if response.StatusCode != successStatus {
		var failure struct {
			Code string `json:"error"`
		}
		if err := decoder.Decode(&failure); err != nil || failure.Code == "" {
			failure.Code = "invalid_response"
		}
		return &mediaServerHTTPRejection{status: response.StatusCode, code: failure.Code}
	}
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
