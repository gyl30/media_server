package main

import (
	"bytes"
	"context"
	"errors"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/emiago/sipgo"
	"github.com/emiago/sipgo/sip"
)

// The peer speaks SIP over UDP; the HTTP boundary stands in for the media server.
func testLivePeer(t *testing.T, s *infrastructureServer, deviceID, channelID string, byeStatus ...int) *atomic.Int32 {
	t.Helper()
	return testLivePeerWithAnswer(t, s, deviceID, channelID, nil, byeStatus...)
}

func testLivePeerWithAnswer(t *testing.T, s *infrastructureServer, deviceID, channelID string, rewrite func([]byte) []byte, byeStatus ...int) *atomic.Int32 {
	t.Helper()
	listener, err := net.ListenPacket("udp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	ua, err := sipgo.NewUA()
	if err != nil {
		t.Fatal(err)
	}
	server, err := sipgo.NewServer(ua)
	if err != nil {
		t.Fatal(err)
	}
	client, err := sipgo.NewClient(ua, sipgo.WithClientConnectionAddr(listener.LocalAddr().String()))
	if err != nil {
		t.Fatal(err)
	}
	host, portText, _ := net.SplitHostPort(listener.LocalAddr().String())
	port, _ := strconv.Atoi(portText)
	contact := sip.Uri{Scheme: "sip", User: deviceID, Host: host, Port: port}
	dialogs := sipgo.NewDialogServerCache(client, sip.ContactHeader{Address: contact})
	invites := new(atomic.Int32)
	var byeReceived atomic.Bool
	server.OnInvite(func(request *sip.Request, transaction sip.ServerTransaction) {
		invites.Add(1)
		dialog, err := dialogs.ReadInvite(request, transaction)
		if err != nil {
			t.Error(err)
			return
		}
		answer := bytes.ReplaceAll(request.Body(), []byte("a=recvonly"), []byte("a=sendonly"))
		if rewrite != nil {
			answer = rewrite(answer)
		}
		if err := dialog.RespondSDP(answer); err != nil && err.Error() != "No ACK received" {
			t.Error(err)
		}
	})
	server.OnAck(func(request *sip.Request, transaction sip.ServerTransaction) {
		// BYE may remove the dialog before sipgo dispatches a late ACK.
		if err := dialogs.ReadAck(request, transaction); err != nil && !(byeReceived.Load() && errors.Is(err, sipgo.ErrDialogDoesNotExists)) {
			t.Error(err)
		}
	})
	server.OnBye(func(request *sip.Request, transaction sip.ServerTransaction) {
		byeReceived.Store(true)
		if len(byeStatus) != 0 {
			_ = transaction.Respond(sip.NewResponseFromRequest(request, byeStatus[0], "Device unavailable", nil))
			return
		}
		if err := dialogs.ReadBye(request, transaction); err != nil {
			t.Error(err)
		}
	})
	done := make(chan error, 1)
	go func() { done <- server.ServeUDP(listener) }()
	t.Cleanup(func() { _ = listener.Close(); <-done; _ = ua.Close() })
	if err := s.live.sip.deviceStore.create(t.Context(), gbDevice{deviceID: deviceID, name: "camera"}); err != nil {
		t.Fatal(err)
	}
	s.live.sip.devices.register(registeredDevice{id: deviceID, contact: contact, remoteEndpoint: listener.LocalAddr().String(), online: true, expiresAt: time.Now().Add(time.Hour), lastHeartbeat: time.Now()})
	s.live.sip.channels.beginQuery(deviceID, 1)
	response := catalogResponse{DeviceID: deviceID, SN: 1, SumNum: 1}
	response.DeviceList.Num = 1
	response.DeviceList.Items = []catalogChannel{{DeviceID: channelID, Status: "ON"}}
	if err := s.live.sip.channels.apply(response); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { s.live.shutdown(context.Background()) })
	return invites
}

func TestConcurrentLiveReusesOneUpstream(t *testing.T) {
	s := testInfrastructure(t)
	var creates atomic.Int32
	created := make(chan struct{})
	releaseCreate := make(chan struct{})
	var release sync.Once
	t.Cleanup(func() { release.Do(func() { close(releaseCreate) }) })
	media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		if request.URL.Path == "/gb28181/receiver/create" {
			if creates.Add(1) == 1 {
				close(created)
			}
			<-releaseCreate
			writeJSON(writer, http.StatusCreated, map[string]int{"rtp_port": 30000})
		} else {
			writer.WriteHeader(http.StatusNoContent)
		}
	}))
	t.Cleanup(media.Close)
	s.media.server.controlURL = media.URL
	deviceID, channelID := "34020000001320000001", "34020000001320000002"
	invites := testLivePeer(t, s, deviceID, channelID)
	type result struct {
		view liveView
		err  error
	}
	results := make(chan result, 8)
	start := func() { view, err := s.live.startLive(t.Context(), deviceID, channelID); results <- result{view, err} }
	go start()
	select {
	case <-created:
	case <-time.After(3 * time.Second):
		t.Fatal("receiver create not reached")
	}
	for range 7 {
		go start()
	}
	release.Do(func() { close(releaseCreate) })
	var streamID string
	for range 8 {
		select {
		case result := <-results:
			if result.err != nil {
				t.Errorf("play failed: %v", result.err)
				continue
			}
			if streamID == "" {
				streamID = result.view.streamID
			}
			if result.view.streamID != streamID || result.view.state != liveStreaming {
				t.Errorf("different live: %+v", result.view)
			}
		case <-time.After(3 * time.Second):
			t.Fatal("play did not finish")
		}
	}
	if creates.Load() != 1 || invites.Load() != 1 {
		t.Fatalf("receivers=%d INVITEs=%d", creates.Load(), invites.Load())
	}
	view, err := s.live.startLive(t.Context(), deviceID, channelID)
	if err != nil || view.streamID != streamID {
		t.Fatalf("streaming reuse: %+v %v", view, err)
	}
}

func TestLiveAdoptsDeviceSelectedSSRC(t *testing.T) {
	s := testInfrastructure(t)
	var updates []string
	var mu sync.Mutex
	media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		switch request.URL.Path {
		case "/gb28181/receiver/create":
			writeJSON(writer, http.StatusCreated, map[string]int{"rtp_port": 30000})
		case "/gb28181/receiver/update":
			body, _ := io.ReadAll(request.Body)
			mu.Lock()
			updates = append(updates, string(body))
			mu.Unlock()
			writer.WriteHeader(http.StatusNoContent)
		default:
			writer.WriteHeader(http.StatusNoContent)
		}
	}))
	t.Cleanup(media.Close)
	s.media.server.controlURL = media.URL
	deviceID, channelID := "34020000001320000001", "34020000001320000002"
	// 设备改用自己的 SSRC，并且不返回 f= 字段、使用非 Play 的会话名。
	testLivePeerWithAnswer(t, s, deviceID, channelID, func(answer []byte) []byte {
		lines := strings.Split(string(answer), "\r\n")
		kept := lines[:0]
		for _, line := range lines {
			switch {
			case strings.HasPrefix(line, "y="):
				kept = append(kept, "y=0123456789")
			case strings.HasPrefix(line, "f="):
			case strings.HasPrefix(line, "s="):
				kept = append(kept, "s=Embedded Net DVR")
			default:
				kept = append(kept, line)
			}
		}
		return []byte(strings.Join(kept, "\r\n"))
	})
	view, err := s.live.startLive(t.Context(), deviceID, channelID)
	if err != nil || view.state != liveStreaming {
		t.Fatalf("live with device SSRC failed: %+v %v", view, err)
	}
	mu.Lock()
	defer mu.Unlock()
	if len(updates) != 1 || !strings.Contains(updates[0], `"ssrc":123456789`) || !strings.Contains(updates[0], view.streamID) {
		t.Fatalf("receiver SSRC not updated: %v", updates)
	}
}
