package main

import (
	"context"
	"encoding/hex"
	"encoding/json"
	"encoding/xml"
	"fmt"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/emiago/sipgo"
	"github.com/emiago/sipgo/sip"
)

func TestPTZHTTPInvalidRequest(t *testing.T) {
	s := testInfrastructure(t)
	for _, body := range []string{`{}`, `{"command":"left","speed":0}`, `{"command":"left","speed":256}`, `{"command":"left","speed":-1}`, `{"command":"zoom_in","speed":16}`, `{"command":"zoom_out","speed":0}`, `{"command":"stop","speed":1}`, `{"command":"focus","speed":1}`, `{"command":"left","speed":1,"duration":500}`, `{"command":"left","speed":1.5}`} {
		apiRequest(t, s.handler(), "POST", "/api/devices/"+queryTestDeviceID+"/channels/"+presetQueryChannelID+"/ptz", body, http.StatusBadRequest)
	}
	apiRequest(t, s.handler(), "POST", "/api/devices/invalid/channels/"+presetQueryChannelID+"/ptz", `{"command":"stop"}`, http.StatusBadRequest)
	apiRequest(t, s.handler(), "POST", "/api/devices/"+queryTestDeviceID+"/channels/invalid/ptz", `{"command":"stop"}`, http.StatusBadRequest)
	if s.live.sip.messageSN.Load() != 0 || len(s.live.ptzOwners) != 0 {
		t.Fatal("invalid input allocated operation")
	}
}

func TestPTZEncoding(t *testing.T) {
	for _, test := range []struct {
		command string
		speed   int
		code    string
	}{
		{"right", 64, "A50F0001400000F5"}, {"left", 64, "A50F0002400000F6"}, {"down", 64, "A50F0004004000F8"}, {"up", 64, "A50F0008004000FC"},
		{"down_right", 64, "A50F000540400039"}, {"down_left", 64, "A50F00064040003A"}, {"up_right", 64, "A50F00094040003D"}, {"up_left", 64, "A50F000A4040003E"},
		{"zoom_in", 4, "A50F001000004004"}, {"zoom_out", 4, "A50F002000004014"}, {"stop", 0, "A50F0000000000B4"},
		{"left", 255, "A50F0002FF0000B5"}, {"zoom_in", 15, "A50F00100000F0B4"},
	} {
		got, err := encodePTZ(test.command, test.speed)
		if err != nil || got != test.code {
			t.Errorf("%s/%d: %s %v want%s", test.command, test.speed, got, err, test.code)
		}
		packet, err := hex.DecodeString(got)
		if err != nil || len(packet) != 8 {
			t.Fatalf("invalid packet %q", got)
		}
		var checksum byte
		for _, value := range packet[:7] {
			checksum += value
		}
		if checksum != packet[7] {
			t.Errorf("bad checksum: %s", got)
		}
	}
}

func TestPTZPreconditions(t *testing.T) {
	for _, test := range []struct {
		name                              string
		create, online, channel, stopping bool
		status                            int
	}{
		{name: "unknown", status: http.StatusNotFound}, {name: "offline", create: true, status: http.StatusConflict}, {name: "missing channel", create: true, online: true, status: http.StatusNotFound}, {name: "stopping", create: true, online: true, channel: true, stopping: true, status: http.StatusConflict},
	} {
		t.Run(test.name, func(t *testing.T) {
			s := testInfrastructure(t)
			if test.create {
				if err := s.live.sip.deviceStore.create(t.Context(), gbDevice{deviceID: queryTestDeviceID, name: "PTZ test"}); err != nil {
					t.Fatal(err)
				}
			}
			if test.online {
				s.live.sip.devices.register(registeredDevice{id: queryTestDeviceID, online: true, expiresAt: time.Now().Add(time.Hour)})
			}
			if test.channel {
				addPresetQueryChannels(t, s, presetQueryChannelID)
			}
			if test.stopping {
				s.live.stoppingDevices[queryTestDeviceID] = struct{}{}
			}
			for _, body := range []string{`{"command":"left","speed":64}`, `{"command":"preset_set","preset_id":1}`} {
				apiRequest(t, s.handler(), "POST", "/api/devices/"+queryTestDeviceID+"/channels/"+presetQueryChannelID+"/ptz", body, test.status)
			}
			if len(s.live.ptzOwners) != 0 || s.live.sip.messageSN.Load() != 0 {
				t.Fatal("precondition allocated PTZ")
			}
		})
	}
}

func TestPTZAcceptedAndFailures(t *testing.T) {
	for _, test := range []struct {
		name, command                          string
		speed, startStatus, stopStatus, status int
		reason                                 string
		lateStart                              bool
	}{
		{name: "accepted", command: "left", speed: 64, startStatus: 200, stopStatus: 200, status: 200},
		{name: "STOP only", command: "stop", stopStatus: 200, status: 200},
		{name: "START rejected still STOP", command: "left", speed: 64, startStatus: 403, stopStatus: 200, status: 502, reason: "ptz_start_failed"},
		{name: "START 200 lost still STOP", command: "left", speed: 64, stopStatus: 200, status: 502, reason: "ptz_start_unconfirmed"},
		{name: "START 200 after STOP", command: "left", speed: 64, startStatus: 200, stopStatus: 200, status: 502, reason: "ptz_start_unconfirmed", lateStart: true},
		{name: "START unknown STOP rejected", command: "left", speed: 64, stopStatus: 403, status: 502, reason: "ptz_stop_unconfirmed"},
		{name: "STOP rejected", command: "left", speed: 64, startStatus: 200, stopStatus: 403, status: 502, reason: "ptz_stop_unconfirmed"},
		{name: "STOP timeout", command: "stop", status: 502, reason: "ptz_stop_unconfirmed"},
	} {
		t.Run(test.name, func(t *testing.T) {
			s := testInfrastructure(t)
			var mu sync.Mutex
			var commands []ptzControl
			stopReceived, startFinished := make(chan struct{}), make(chan struct{})
			var stopOnce sync.Once
			newDeviceMessagePeer(t, s, func(request *sip.Request, tx sip.ServerTransaction, _ *deviceQueryPeer) {
				var control ptzControl
				if err := xml.Unmarshal(request.Body(), &control); err != nil {
					t.Error(err)
					return
				}
				mu.Lock()
				commands = append(commands, control)
				mu.Unlock()
				if control.Command != "DeviceControl" || control.DeviceID != presetQueryChannelID || control.Info.Priority != 5 || control.SN <= 0 || request.To().Address.User != queryTestDeviceID {
					t.Errorf("wrong PTZ target/body %+v", control)
				}
				status := test.startStatus
				if control.PTZCommand == "A50F0000000000B4" {
					stopOnce.Do(func() { close(stopReceived) })
					status = test.stopStatus
				} else if test.lateStart {
					defer close(startFinished)
					select {
					case <-stopReceived:
					case <-time.After(2 * time.Second):
						t.Error("STOP not received before late START response")
						return
					}
				}
				if status != 0 {
					if err := tx.Respond(sip.NewResponseFromRequest(request, status, "test", nil)); err != nil {
						t.Error(err)
					}
				}
			})
			addPresetQueryChannels(t, s, presetQueryChannelID)
			registration, _ := s.live.sip.devices.get(queryTestDeviceID)
			registration.lastHeartbeat = time.Now()
			s.live.sip.devices.register(registration)
			response := apiRequest(t, s.handler(), "POST", "/api/devices/"+queryTestDeviceID+"/channels/"+presetQueryChannelID+"/ptz", fmt.Sprintf(`{"command":%q,"speed":%d}`, test.command, test.speed), test.status)
			if test.lateStart {
				select {
				case <-startFinished:
				case <-time.After(time.Second):
					t.Fatal("late START response did not finish")
				}
			}
			if test.reason != "" {
				if !strings.Contains(response.Body.String(), test.reason) {
					t.Fatal(response.Body.String())
				}
			} else {
				var result ptzResponse
				if err := json.Unmarshal(response.Body.Bytes(), &result); err != nil {
					t.Fatal(err)
				}
				if result.DeviceID != queryTestDeviceID || result.ChannelID != presetQueryChannelID || result.Command != test.command || result.Stop != "accepted" || result.PhysicalResult != "unconfirmed" || (test.command != "stop" && result.Start != "accepted") {
					t.Fatalf("response %+v", result)
				}
			}
			if test.name == "START 200 lost still STOP" {
				// 跨过默认 UDP T1=500ms：STOP 后不得再有旧 START 重传。
				select {
				case <-time.After(600 * time.Millisecond):
				case <-t.Context().Done():
					t.Fatal("test cancelled")
				}
			}
			mu.Lock()
			defer mu.Unlock()
			starts, stops := 0, 0
			for _, control := range commands {
				if control.PTZCommand == "A50F0000000000B4" {
					stops++
				} else {
					starts++
				}
			}
			if test.command == "stop" {
				if starts != 0 || stops < 1 {
					t.Fatalf("commands %+v", commands)
				}
			} else if starts != 1 || stops < 1 || commands[len(commands)-1].PTZCommand != "A50F0000000000B4" || commands[0].SN == commands[len(commands)-1].SN {
				t.Fatalf("commands %+v", commands)
			}
			if len(s.live.ptzOwners) != 0 {
				t.Fatal("owner retained")
			}
		})
	}
}

func TestPTZOwnerCancellationAndIndependentSTOP(t *testing.T) {
	for _, reason := range []string{"manual STOP", "HTTP cancel", "device unregister", "registration expired", "heartbeat expired", "device delete", "STOP during STOP", "offline during STOP", "server shutdown during STOP"} {
		t.Run(reason, func(t *testing.T) {
			s := testInfrastructure(t)
			startReceived, stopReceived, releaseStop := make(chan struct{}), make(chan struct{}), make(chan struct{})
			var startOnce, stopOnce, releaseOnce sync.Once
			defer releaseOnce.Do(func() { close(releaseStop) })
			peer := newDeviceMessagePeer(t, s, func(request *sip.Request, tx sip.ServerTransaction, _ *deviceQueryPeer) {
				var control ptzControl
				if err := xml.Unmarshal(request.Body(), &control); err != nil {
					t.Error(err)
					return
				}
				if control.PTZCommand == "A50F0000000000B4" {
					stopOnce.Do(func() { close(stopReceived) })
					<-releaseStop
				} else {
					startOnce.Do(func() { close(startReceived) })
				}
				if err := tx.Respond(sip.NewResponseFromRequest(request, 200, "OK", nil)); err != nil {
					t.Error(err)
				}
			})
			addPresetQueryChannels(t, s, presetQueryChannelID)
			path := "/api/devices/" + queryTestDeviceID + "/channels/" + presetQueryChannelID + "/ptz"
			registration, _ := s.live.sip.devices.get(queryTestDeviceID)
			registration.lastHeartbeat = time.Now()
			s.live.sip.devices.register(registration)
			ctx, cancel := context.WithCancel(t.Context())
			defer cancel()
			body := `{"command":"left","speed":64}`
			stopOnly := strings.Contains(reason, "during STOP")
			if stopOnly {
				body = `{"command":"stop"}`
			}
			completed := make(chan *httptest.ResponseRecorder, 1)
			go func() {
				response := httptest.NewRecorder()
				request := httptest.NewRequest("POST", path, strings.NewReader(body)).WithContext(ctx)
				request.Header.Set("Content-Type", "application/json")
				s.handler().ServeHTTP(response, request)
				completed <- response
			}()
			barrier := startReceived
			if stopOnly {
				barrier = stopReceived
			}
			select {
			case <-barrier:
			case <-time.After(time.Second):
				t.Fatal("PTZ not received")
			}
			if reason == "manual STOP" || reason == "STOP during STOP" {
				apiRequest(t, s.handler(), "POST", path, `{"command":"right","speed":64}`, http.StatusConflict)
				for range 2 {
					response := apiRequest(t, s.handler(), "POST", path, `{"command":"stop"}`, http.StatusAccepted)
					if !strings.Contains(response.Body.String(), `"stop":"requested"`) {
						t.Fatal(response.Body.String())
					}
				}
			} else if reason == "HTTP cancel" {
				cancel()
			} else if reason == "device unregister" {
				registration, _ := s.live.sip.devices.get(queryTestDeviceID)
				request := sip.NewRequest(sip.REGISTER, sip.Uri{Scheme: "sip", User: peer.platformID, Host: "127.0.0.1"})
				identity := sip.Uri{Scheme: "sip", User: queryTestDeviceID, Host: s.live.sip.cfg.sipDomain}
				request.AppendHeader(&sip.FromHeader{Address: identity, Params: sip.NewParams()})
				request.AppendHeader(&sip.ToHeader{Address: identity})
				request.AppendHeader(&sip.ContactHeader{Address: registration.contact})
				expires := sip.ExpiresHeader(0)
				request.AppendHeader(&expires)
				request.SetTransport("UDP")
				request.SetDestination(peer.platform)
				registrationContext, stop := context.WithTimeout(t.Context(), time.Second)
				defer stop()
				challenge, err := peer.client.Do(registrationContext, request)
				if err != nil {
					t.Fatal(err)
				}
				response, err := peer.client.DoDigestAuth(registrationContext, request, challenge, sipgo.DigestAuth{Username: queryTestDeviceID, Password: s.live.sip.cfg.sipPassword})
				if err != nil {
					t.Fatal(err)
				}
				if response.StatusCode != 200 {
					t.Fatalf("unregister SIP%d", response.StatusCode)
				}
			} else if reason == "registration expired" || reason == "heartbeat expired" || reason == "offline during STOP" {
				delta := 2 * time.Hour
				if reason == "heartbeat expired" {
					delta = s.live.sip.cfg.heartbeatTimeout
				}
				s.live.sip.expireDevices(time.Now().Add(delta))
			} else if reason == "device delete" {
				apiRequest(t, s.handler(), "DELETE", "/api/devices/"+queryTestDeviceID, "", http.StatusNoContent)
			} else {
				// 实际 serve 的关闭路径必须取消 owner，但不能影响其独立 STOP。
				serveContext, stopServe := context.WithCancel(t.Context())
				listener, err := net.Listen("tcp", "127.0.0.1:0")
				if err != nil {
					t.Fatal(err)
				}
				s.httpListen = listener.Addr().String()
				_ = listener.Close()
				serveDone := make(chan error, 1)
				go func() { serveDone <- s.serve(serveContext) }()
				client := &http.Client{Timeout: time.Second}
				deadline := time.Now().Add(time.Second)
				for {
					response, err := client.Get("http://" + s.httpListen + "/api/devices")
					if err == nil {
						_ = response.Body.Close()
						break
					}
					if time.Now().After(deadline) {
						stopServe()
						t.Fatal("HTTP serve not ready")
					}
					time.Sleep(5 * time.Millisecond)
				}
				stopServe()
				select {
				case err := <-serveDone:
					if err != nil {
						t.Fatal(err)
					}
				case <-time.After(time.Second):
					t.Fatal("HTTP serve did not stop")
				}
			}
			select {
			case <-stopReceived:
			case <-time.After(time.Second):
				t.Fatal("independent STOP not received")
			}
			s.live.mu.Lock()
			owners := len(s.live.ptzOwners)
			s.live.mu.Unlock()
			if owners != 1 {
				t.Fatal("STOP owner released before transaction completed")
			}
			releaseOnce.Do(func() { close(releaseStop) })
			select {
			case response := <-completed:
				if response.Code != http.StatusOK && !(response.Code == http.StatusBadGateway && strings.Contains(response.Body.String(), "ptz_start_unconfirmed")) {
					t.Fatalf("HTTP%d: %s", response.Code, response.Body.String())
				}
			case <-time.After(2 * time.Second):
				t.Fatal("owner did not finish")
			}
			s.live.mu.Lock()
			owners = len(s.live.ptzOwners)
			s.live.mu.Unlock()
			if owners != 0 {
				t.Fatal("owner retained")
			}
		})
	}
}

func TestPTZDifferentChannelsRunConcurrently(t *testing.T) {
	s := testInfrastructure(t)
	received := make(chan string, 4)
	release := make(chan struct{})
	var releaseOnce sync.Once
	defer releaseOnce.Do(func() { close(release) })
	newDeviceMessagePeer(t, s, func(request *sip.Request, tx sip.ServerTransaction, _ *deviceQueryPeer) {
		var control ptzControl
		if err := xml.Unmarshal(request.Body(), &control); err != nil {
			t.Error(err)
			return
		}
		received <- control.DeviceID
		<-release
		if err := tx.Respond(sip.NewResponseFromRequest(request, 200, "OK", nil)); err != nil {
			t.Error(err)
		}
	})
	channels := []string{presetQueryChannelID, "34020000001320000003"}
	addPresetQueryChannels(t, s, channels...)
	completed := make(chan int, 2)
	for _, channelID := range channels {
		go func() {
			response := httptest.NewRecorder()
			request := httptest.NewRequest("POST", "/api/devices/"+queryTestDeviceID+"/channels/"+channelID+"/ptz", strings.NewReader(`{"command":"stop"}`))
			request.Header.Set("Content-Type", "application/json")
			s.handler().ServeHTTP(response, request)
			completed <- response.Code
		}()
	}
	seen := make(map[string]bool)
	for range 2 {
		select {
		case channelID := <-received:
			seen[channelID] = true
		case <-time.After(time.Second):
			t.Fatal("different channels did not reach peer concurrently")
		}
	}
	if len(seen) != 2 {
		t.Fatalf("wrong targets %v", seen)
	}
	s.live.mu.Lock()
	owners := len(s.live.ptzOwners)
	s.live.mu.Unlock()
	if owners != 2 {
		t.Fatalf("owners=%d want2", owners)
	}
	releaseOnce.Do(func() { close(release) })
	for range 2 {
		select {
		case status := <-completed:
			if status != 200 {
				t.Fatalf("channel HTTP%d", status)
			}
		case <-time.After(time.Second):
			t.Fatal("different channels did not complete")
		}
	}
	if len(s.live.ptzOwners) != 0 {
		t.Fatal("owners retained")
	}
}

func TestPTZHTTPDisconnectStillStops(t *testing.T) {
	s := testInfrastructure(t)
	started, stopped := make(chan struct{}), make(chan struct{})
	newDeviceMessagePeer(t, s, func(request *sip.Request, tx sip.ServerTransaction, _ *deviceQueryPeer) {
		var control ptzControl
		if err := xml.Unmarshal(request.Body(), &control); err != nil {
			t.Error(err)
			return
		}
		if err := tx.Respond(sip.NewResponseFromRequest(request, 200, "OK", nil)); err != nil {
			t.Error(err)
		}
		if control.PTZCommand == "A50F0000000000B4" {
			close(stopped)
		} else {
			close(started)
		}
	})
	addPresetQueryChannels(t, s, presetQueryChannelID)
	server := httptest.NewServer(s.handler())
	defer server.Close()
	ctx, cancel := context.WithCancel(t.Context())
	defer cancel()
	request, err := http.NewRequestWithContext(ctx, "POST", server.URL+"/api/devices/"+queryTestDeviceID+"/channels/"+presetQueryChannelID+"/ptz", strings.NewReader(`{"command":"left","speed":64}`))
	if err != nil {
		t.Fatal(err)
	}
	request.Header.Set("Content-Type", "application/json")
	done := make(chan error, 1)
	go func() {
		response, err := server.Client().Do(request)
		if response != nil {
			_ = response.Body.Close()
		}
		done <- err
	}()
	select {
	case <-started:
	case <-time.After(time.Second):
		t.Fatal("START not received")
	}
	cancel()
	select {
	case err := <-done:
		if err == nil {
			t.Fatal("HTTP cancellation not observed")
		}
	case <-time.After(time.Second):
		t.Fatal("HTTP did not cancel")
	}
	select {
	case <-stopped:
	case <-time.After(time.Second):
		t.Fatal("HTTP disconnect prevented STOP")
	}
	// Server.Close 等待真实 HTTP handler 完成，不能只依据 peer 收到 STOP 就宣称 owner 已释放。
	server.Close()
	s.live.mu.Lock()
	owners := len(s.live.ptzOwners)
	s.live.mu.Unlock()
	if owners != 0 {
		t.Fatal("owner retained after disconnect")
	}
}
