package main

import (
	"bytes"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"strings"
	"testing"
	"time"

	"github.com/emiago/sipgo/sip"
	"github.com/google/uuid"
)

func TestGBHTTPListsDevicesAndChannels(t *testing.T) {
	platform := newTestSIPServer(t, "127.0.0.1:0")
	now := time.Date(2026, 9, 14, 12, 0, 0, 0, time.UTC)
	platform.now = func() time.Time { return now }
	secondDeviceID := "34020000001320000009"
	for index, deviceID := range []string{secondDeviceID, testDeviceID} {
		platform.devices.register(registeredDevice{
			id: deviceID, contact: sip.Uri{Scheme: "sip", User: deviceID, Host: "127.0.0.1"},
			remoteEndpoint: "127.0.0.1:5060", expiresAt: now.Add(time.Hour),
			lastHeartbeat: now.Add(-time.Duration(index+1) * time.Minute), online: true,
		})
	}
	platform.channels.beginQuery(testDeviceID, 1)
	if err := platform.channels.apply(catalogResponse{
		CmdType: "Catalog", SN: 1, DeviceID: testDeviceID, SumNum: 2,
		DeviceList: struct {
			Num   int              `xml:"Num,attr"`
			Items []catalogChannel `xml:"Item"`
		}{Num: 2, Items: []catalogChannel{
			{DeviceID: "34020000001320000003", Name: "Camera 2", ParentID: testDeviceID, Status: "OFF"},
			{DeviceID: testChannelID, Name: "Camera 1", ParentID: testDeviceID, Status: "ON"},
		}},
	}); err != nil {
		t.Fatalf("apply catalog error = %v", err)
	}
	registry := newMediaServerRegistry()
	allocator, err := newSSRCAllocator(platform.cfg.sipDomain)
	if err != nil {
		t.Fatalf("newSSRCAllocator() error = %v", err)
	}
	server := newTestInfrastructureServer(t, testConfig(), registry, slog.New(slog.NewTextHandler(io.Discard, nil)))
	server.live = newLiveService(platform, registry, newMediaServerHTTPClient(time.Second), allocator,
		slog.New(slog.NewTextHandler(io.Discard, nil)))

	response := sourceRequest(t, server.handler(), http.MethodGet, "/api/devices", "", "")
	if response.Code != http.StatusOK {
		t.Fatalf("device list status/body = %d %s", response.Code, response.Body.String())
	}
	if strings.Contains(response.Body.String(), "remote_endpoint") || strings.Contains(response.Body.String(), "contact") ||
		strings.Contains(response.Body.String(), "expires_at") {
		t.Fatalf("device response leaked transport details: %s", response.Body.String())
	}
	var devices struct {
		Devices []deviceResponse `json:"devices"`
	}
	if err := json.Unmarshal(response.Body.Bytes(), &devices); err != nil || len(devices.Devices) != 2 {
		t.Fatalf("device response = %+v, %v", devices, err)
	}
	if devices.Devices[0].DeviceID != testDeviceID || devices.Devices[1].DeviceID != secondDeviceID ||
		!devices.Devices[0].Online || devices.Devices[0].LastSeen.IsZero() {
		t.Fatalf("devices = %+v", devices.Devices)
	}

	response = sourceRequest(t, server.handler(), http.MethodGet, "/api/devices/"+testDeviceID+"/channels", "", "")
	var channels struct {
		Channels []channelResponse `json:"channels"`
	}
	if response.Code != http.StatusOK {
		t.Fatalf("channel list status/body = %d %s", response.Code, response.Body.String())
	}
	if err := json.Unmarshal(response.Body.Bytes(), &channels); err != nil || len(channels.Channels) != 2 {
		t.Fatalf("channel response = %+v, %v", channels, err)
	}
	if channel := channels.Channels[0]; channel.ChannelID != testChannelID || channel.Name != "Camera 1" ||
		channel.ParentID != testDeviceID || channel.Status != "ON" || channel.Live != nil {
		t.Fatalf("channel = %+v", channel)
	}
	if channels.Channels[1].ChannelID != "34020000001320000003" {
		t.Fatalf("channels are not sorted: %+v", channels.Channels)
	}
	response = sourceRequest(t, server.handler(), http.MethodGet, "/api/devices/"+secondDeviceID+"/channels", "", "")
	if err := json.Unmarshal(response.Body.Bytes(), &channels); err != nil || channels.Channels == nil || len(channels.Channels) != 0 {
		t.Fatalf("empty channel response = %+v, %v", channels, err)
	}
	response = sourceRequest(t, server.handler(), http.MethodGet, "/api/devices/34020000001320000008/channels", "", "")
	if response.Code != http.StatusNotFound {
		t.Fatalf("missing device status/body = %d %s", response.Code, response.Body.String())
	}
	response = sourceRequest(t, server.handler(), http.MethodPost, "/api/devices", "", "")
	if response.Code != http.StatusMethodNotAllowed {
		t.Fatalf("wrong method status = %d", response.Code)
	}
}

func TestGBHTTPStartAndGenerationFencedStop(t *testing.T) {
	device := startLiveTestDevice(t, func(request *sip.Request) ([]byte, int) {
		return []byte(strings.ReplaceAll(string(request.Body()), "a=recvonly", "a=sendonly")), sip.StatusOK
	})
	platform, _ := startRegistrar(t, testConfig())
	registerLiveTestDevice(t, platform, device.addr)
	mediaRegistry, _, _, deletes := startLiveTestMediaServer(t)
	allocator, err := newSSRCAllocator(platform.cfg.sipDomain)
	if err != nil {
		t.Fatalf("newSSRCAllocator() error = %v", err)
	}
	live := newLiveService(platform, mediaRegistry, newMediaServerHTTPClient(time.Second), allocator,
		slog.New(slog.NewTextHandler(io.Discard, nil)))
	server := newTestInfrastructureServer(t, testConfig(), mediaRegistry, slog.New(slog.NewTextHandler(io.Discard, nil)))
	server.live = live
	path := "/api/devices/" + testDeviceID + "/channels/" + testChannelID

	response := sourceRequest(t, server.handler(), http.MethodPost, path+"/start", "", "")
	if response.Code != http.StatusCreated {
		t.Fatalf("start status/body = %d %s", response.Code, response.Body.String())
	}
	var started struct {
		StreamID   string    `json:"stream_id"`
		StreamName string    `json:"stream_name"`
		State      liveState `json:"state"`
	}
	decoder := json.NewDecoder(bytes.NewReader(response.Body.Bytes()))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&started); err != nil ||
		!validUUIDv4(started.StreamID) || started.StreamName != "gb/"+testDeviceID+"/"+testChannelID ||
		started.State != liveStreaming {
		t.Fatalf("start response = %+v, %v", started, err)
	}
	if err := decoder.Decode(&struct{}{}); err != io.EOF {
		t.Fatalf("start response has extra JSON: %v", err)
	}
	if strings.Contains(response.Body.String(), "ssrc") || strings.Contains(response.Body.String(), "rtp_port") {
		t.Fatalf("public start exposed receiver details: %s", response.Body.String())
	}
	instance, ok := mediaRegistry.selectOnline()
	if !ok {
		t.Fatal("media server is not online")
	}
	if _, err := server.runtimes.apply(observedRuntime{
		Kind: "source", ServerID: instance.serverID, InstanceID: instance.instanceID,
		StreamID: started.StreamID, StreamName: started.StreamName, Protocol: "gb28181",
		State: "streaming", Stage: "streaming",
	}); err != nil {
		t.Fatalf("apply GB runtime error = %v", err)
	}
	select {
	case <-device.acks:
	case <-time.After(2 * time.Second):
		t.Fatal("ACK was not received")
	}
	response = sourceRequest(t, server.handler(), http.MethodPost, path+"/stop",
		`{"stream_id":"`+uuid.NewString()+`"}`, "application/json")
	if response.Code != http.StatusConflict || live.len() != 1 || deletes.Load() != 0 {
		t.Fatalf("stale stop status/body live/deletes = %d %s %d/%d", response.Code, response.Body.String(), live.len(), deletes.Load())
	}
	response = sourceRequest(t, server.handler(), http.MethodPost, path+"/stop",
		`{"stream_id":"invalid"}`, "application/json")
	if response.Code != http.StatusBadRequest || live.len() != 1 {
		t.Fatalf("invalid stop status/body live = %d %s %d", response.Code, response.Body.String(), live.len())
	}
	response = sourceRequest(t, server.handler(), http.MethodGet, "/api/devices/"+testDeviceID+"/channels", "", "")
	var channels struct {
		Channels []channelResponse `json:"channels"`
	}
	if err := json.Unmarshal(response.Body.Bytes(), &channels); err != nil || len(channels.Channels) != 1 ||
		channels.Channels[0].Live == nil || channels.Channels[0].Live.StreamID != started.StreamID {
		t.Fatalf("active channel response = %+v, %v", channels, err)
	}
	response = sourceRequest(t, server.handler(), http.MethodPost, path+"/stop",
		`{"stream_id":"`+started.StreamID+`"}`, "application/json")
	if response.Code != http.StatusNoContent || response.Body.Len() != 0 {
		t.Fatalf("stop status/body = %d %s", response.Code, response.Body.String())
	}
	select {
	case <-device.byes:
	case <-time.After(2 * time.Second):
		t.Fatal("BYE was not received")
	}
	if live.len() != 0 || allocator.activeCount() != 0 || deletes.Load() != 1 {
		t.Fatalf("cleanup live=%d ssrc=%d deletes=%d", live.len(), allocator.activeCount(), deletes.Load())
	}
	observed := server.runtimes.snapshot()
	if len(observed) != 1 || observed[0].StreamID != started.StreamID || observed[0].Kind != "source" ||
		observed[0].State != "stopped" {
		t.Fatalf("observed after GB stop = %+v", observed)
	}
}
