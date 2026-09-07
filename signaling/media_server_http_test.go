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
	if len(create) != 4 || create["stream_name"] != "gb/34020000001320000001/34020000001320000002" || create["transport"] != "udp" ||
		create["payload_type"] != float64(96) || create["ssrc"] != float64(200000001) {
		t.Fatalf("create body = %#v", create)
	}

	if err := client.deleteReceiver(context.Background(), instance, endpoint.streamName); err != nil {
		t.Fatalf("deleteReceiver() error = %v", err)
	}
	remove := <-requests
	if len(remove) != 1 || remove["stream_name"] != endpoint.streamName {
		t.Fatalf("delete body = %#v", remove)
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
	request := gb28181ReceiverRequest{streamName: "gb/device/channel", payloadType: 96, ssrc: 200000001}
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
				streamName: "gb/device/channel", payloadType: 96, ssrc: 200000001,
			})
			if err == nil {
				t.Fatal("createUDPReceiver() succeeded")
			}
		})
	}
}
