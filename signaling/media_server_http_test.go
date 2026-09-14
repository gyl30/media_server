package main

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

const testStreamID = "550e8400-e29b-41d4-a716-446655440000"

func TestMediaServerHTTPCreateUDPReceiverAndDelete(t *testing.T) {
	requests := make(chan map[string]any, 2)
	server := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		if request.Header.Get("Content-Type") != "application/json" {
			t.Errorf("Content-Type = %q", request.Header.Get("Content-Type"))
		}
		var body map[string]any
		if err := json.NewDecoder(request.Body).Decode(&body); err != nil {
			t.Errorf("Decode() error = %v", err)
		}
		requests <- body
		switch request.URL.Path {
		case "/gb28181/receiver/create":
			writer.Header().Set("Content-Type", "application/json")
			writer.WriteHeader(http.StatusCreated)
			_, _ = io.WriteString(writer, `{"result":"ok","rtp_port":40000,"rtcp_port":40001}`)
		case "/gb28181/receiver/delete":
			writer.Header().Set("Content-Type", "application/json")
			_, _ = io.WriteString(writer, `{"result":"ok"}`)
		default:
			http.NotFound(writer, request)
		}
	}))
	defer server.Close()

	client := newMediaServerHTTPClient(time.Second)
	instance := mediaServerInstance{controlURL: server.URL, mediaIP: "192.0.2.20"}
	endpoint, err := client.createUDPReceiver(context.Background(), instance, gb28181ReceiverRequest{
		streamID:    testStreamID,
		streamName:  "gb/34020000001320000001/34020000001320000002",
		payloadType: 96,
		ssrc:        200000001,
	})
	if err != nil {
		t.Fatalf("createUDPReceiver() error = %v", err)
	}
	if endpoint.address != instance.mediaIP || endpoint.rtpPort != 40000 || endpoint.rtcpPort != 40001 || endpoint.ssrc != 200000001 {
		t.Fatalf("endpoint = %+v", endpoint)
	}
	create := <-requests
	if len(create) != 5 || create["stream_id"] != testStreamID ||
		create["stream_name"] != "gb/34020000001320000001/34020000001320000002" || create["transport"] != "udp" ||
		create["payload_type"] != float64(96) || create["ssrc"] != float64(200000001) {
		t.Fatalf("create body = %#v", create)
	}

	if err := client.deleteReceiver(context.Background(), instance, endpoint.streamID, endpoint.streamName); err != nil {
		t.Fatalf("deleteReceiver() error = %v", err)
	}
	remove := <-requests
	if len(remove) != 2 || remove["stream_id"] != testStreamID || remove["stream_name"] != endpoint.streamName {
		t.Fatalf("delete body = %#v", remove)
	}
}

func TestMediaServerHTTPCreateAndDeleteRTSPPull(t *testing.T) {
	type receivedRequest struct {
		path string
		body map[string]any
	}
	requests := make(chan receivedRequest, 2)
	server := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		if request.Method != http.MethodPost || request.Header.Get("Content-Type") != "application/json" {
			t.Errorf("request = %s Content-Type %q", request.Method, request.Header.Get("Content-Type"))
		}
		var body map[string]any
		if err := json.NewDecoder(request.Body).Decode(&body); err != nil {
			t.Errorf("Decode() error = %v", err)
		}
		requests <- receivedRequest{path: request.URL.Path, body: body}
		status := http.StatusOK
		if request.URL.Path == "/rtsp/pull/create" {
			status = http.StatusCreated
		}
		writer.Header().Set("Content-Type", "application/json")
		writer.WriteHeader(status)
		_, _ = io.WriteString(writer, `{"result":"ok"}`)
	}))
	defer server.Close()

	username := "admin"
	password := ""
	client := newMediaServerHTTPClient(time.Second)
	instance := mediaServerInstance{controlURL: server.URL}
	command := rtspPullCreateRequest{
		StreamID: testStreamID, StreamName: "live/camera", URL: "rtsp://192.0.2.10/live", Username: &username, Password: &password,
	}
	if err := client.createRTSPPull(context.Background(), instance, command); err != nil {
		t.Fatalf("createRTSPPull() error = %v", err)
	}
	create := <-requests
	if create.path != "/rtsp/pull/create" || len(create.body) != 5 || create.body["stream_id"] != testStreamID ||
		create.body["stream_name"] != command.StreamName ||
		create.body["url"] != command.URL || create.body["username"] != username || create.body["password"] != password {
		t.Fatalf("create request = %#v", create)
	}

	if err := client.deleteRTSPPull(context.Background(), instance, command.StreamID, command.StreamName); err != nil {
		t.Fatalf("deleteRTSPPull() error = %v", err)
	}
	remove := <-requests
	if remove.path != "/rtsp/pull/delete" || len(remove.body) != 2 || remove.body["stream_id"] != testStreamID ||
		remove.body["stream_name"] != command.StreamName {
		t.Fatalf("delete request = %#v", remove)
	}
}

func TestMediaServerHTTPDistinguishesRejectionAndNetworkFailure(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, _ *http.Request) {
		writer.Header().Set("Content-Type", "application/json")
		writer.WriteHeader(http.StatusInternalServerError)
		_, _ = io.WriteString(writer, `{"error":"operation_failed"}`)
	}))
	defer server.Close()

	client := newMediaServerHTTPClient(time.Second)
	request := gb28181ReceiverRequest{streamID: testStreamID, streamName: "gb/device/channel", payloadType: 96, ssrc: 200000001}
	_, err := client.createUDPReceiver(context.Background(), mediaServerInstance{controlURL: server.URL, mediaIP: "127.0.0.1"}, request)
	var rejection *mediaServerHTTPRejection
	if !errors.As(err, &rejection) || rejection.status != http.StatusInternalServerError || rejection.code != "operation_failed" {
		t.Fatalf("rejection = %#v, error = %v", rejection, err)
	}

	_, err = client.createUDPReceiver(context.Background(), mediaServerInstance{controlURL: "http://127.0.0.1:1", mediaIP: "127.0.0.1"}, request)
	if err == nil || errors.As(err, &rejection) {
		t.Fatalf("network error = %v", err)
	}
}

func TestMediaServerHTTPRejectsInvalidCreateResponse(t *testing.T) {
	for _, body := range []string{
		`{"result":"ok","rtp_port":40001,"rtcp_port":40002}`,
		`{"result":"ok","rtp_port":40000,"rtcp_port":40002}`,
		`{"result":"ok","rtp_port":0,"rtcp_port":1}`,
		`{"result":"ok"}`,
	} {
		t.Run(body, func(t *testing.T) {
			server := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, _ *http.Request) {
				writer.Header().Set("Content-Type", "application/json")
				writer.WriteHeader(http.StatusCreated)
				_, _ = io.WriteString(writer, body)
			}))
			defer server.Close()
			client := newMediaServerHTTPClient(time.Second)
			_, err := client.createUDPReceiver(context.Background(), mediaServerInstance{controlURL: server.URL, mediaIP: "127.0.0.1"}, gb28181ReceiverRequest{
				streamID: testStreamID, streamName: "gb/device/channel", payloadType: 96, ssrc: 200000001,
			})
			if err == nil {
				t.Fatal("createUDPReceiver() succeeded")
			}
		})
	}
}
