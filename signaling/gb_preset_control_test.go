package main

import (
	"context"
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

func TestPresetControlEncoding(t *testing.T) {
	for _, test := range []struct {
		command string
		id      int
		code    string
	}{
		{"preset_set", 1, "A50F008100010036"}, {"preset_goto", 1, "A50F008200010037"}, {"preset_remove", 1, "A50F008300010038"},
		{"preset_set", 255, "A50F008100FF0034"}, {"preset_goto", 255, "A50F008200FF0035"}, {"preset_remove", 255, "A50F008300FF0036"},
	} {
		got, err := encodePTZ(test.command, test.id)
		if err != nil || got != test.code {
			t.Errorf("%s/%d: %q %v want %s", test.command, test.id, got, err, test.code)
		}
	}
	for _, command := range []string{"preset_set", "preset_goto", "preset_remove"} {
		for _, id := range []int{-1, 0, 256} {
			if _, err := encodePTZ(command, id); err == nil {
				t.Errorf("accepted %s/%d", command, id)
			}
		}
	}
}

func TestPresetControlInvalidFields(t *testing.T) {
	s := testInfrastructure(t)
	path := "/api/devices/" + queryTestDeviceID + "/channels/" + presetQueryChannelID + "/ptz"
	for _, body := range []string{
		`{"command":"preset_set"}`, `{"command":"preset_set","preset_id":0}`, `{"command":"preset_set","preset_id":256}`,
		`{"command":"preset_set","preset_id":1.5}`, `{"command":"preset_set","preset_id":"1"}`, `{"command":"preset_set","preset_id":null}`,
		`{"command":"preset_set","preset_id":1,"speed":0}`, `{"command":"preset_set","preset_id":1,"speed":null}`, `{"command":"preset_goto","preset_id":1,"speed":64}`, `{"command":"preset_remove","preset_id":1,"name":"x"}`,
		`{"command":"left","speed":1,"preset_id":0}`, `{"command":"left","speed":1,"preset_id":1}`, `{"command":"stop","preset_id":1}`, `{"command":"stop","preset_id":null}`, `{"command":"zoom_in","speed":1,"preset_id":1}`,
	} {
		apiRequest(t, s.handler(), "POST", path, body, http.StatusBadRequest)
	}
	if len(s.live.ptzOwners) != 0 || s.live.sip.messageSN.Load() != 0 {
		t.Fatal("invalid input allocated operation")
	}
}

func TestPresetControlAccepted(t *testing.T) {
	for _, command := range []string{"preset_set", "preset_goto", "preset_remove"} {
		t.Run(command, func(t *testing.T) {
			s := testInfrastructure(t)
			var mu sync.Mutex
			var commands []ptzControl
			newDeviceMessagePeer(t, s, func(req *sip.Request, tx sip.ServerTransaction, _ *deviceQueryPeer) {
				var control ptzControl
				if err := xml.Unmarshal(req.Body(), &control); err != nil {
					t.Error(err)
					return
				}
				if control.Command != "DeviceControl" || control.DeviceID != presetQueryChannelID || control.Info.Priority != 5 || req.To().Address.User != queryTestDeviceID {
					t.Errorf("wrong target %+v", control)
				}
				mu.Lock()
				commands = append(commands, control)
				mu.Unlock()
				if err := tx.Respond(sip.NewResponseFromRequest(req, 200, "OK", nil)); err != nil {
					t.Error(err)
				}
			})
			addPresetQueryChannels(t, s, presetQueryChannelID)
			path := "/api/devices/" + queryTestDeviceID + "/channels/" + presetQueryChannelID + "/ptz"
			started := time.Now()
			response := apiRequest(t, s.handler(), "POST", path, fmt.Sprintf(`{"command":%q,"preset_id":1}`, command), 200)
			t.Logf("预置位SIP200至HTTP完成路径总耗时 %s（无300ms点动等待）", time.Since(started))
			var result map[string]any
			if err := json.Unmarshal(response.Body.Bytes(), &result); err != nil {
				t.Fatal(err)
			}
			if len(result) != 6 || result["device_id"] != queryTestDeviceID || result["channel_id"] != presetQueryChannelID || result["command"] != command || result["preset_id"] != float64(1) || result["accepted"] != true || result["physical_result"] != "unconfirmed" {
				t.Fatalf("response %s", response.Body.String())
			}
			// 正常成功既不等待300ms，也不主动STOP；跨UDP T1检查已完成事务无重传。
			select {
			case <-time.After(550 * time.Millisecond):
			case <-t.Context().Done():
				t.Fatal("cancelled")
			}
			mu.Lock()
			want, _ := encodePTZ(command, 1)
			valid := len(commands) == 1 && commands[0].PTZCommand == want
			mu.Unlock()
			if !valid {
				t.Fatalf("unexpected commands %+v", commands)
			}
			if len(s.live.ptzOwners) != 0 {
				t.Fatal("owner retained")
			}
			// 已成功释放后，显式STOP自己拥有事务，不能误报202。
			apiRequest(t, s.handler(), "POST", path, `{"command":"stop"}`, 200)
			mu.Lock()
			defer mu.Unlock()
			if len(commands) != 2 || commands[1].PTZCommand != "A50F0000000000B4" {
				t.Fatalf("explicit STOP missing %+v", commands)
			}
		})
	}
}

func TestPresetControlFailuresStillStop(t *testing.T) {
	for _, test := range []struct {
		name                     string
		presetStatus, stopStatus int
		reason                   string
	}{
		{"reject", 403, 200, "preset_control_unconfirmed"}, {"timeout", 0, 200, "preset_control_unconfirmed"}, {"STOP reject", 403, 403, "ptz_stop_unconfirmed"},
	} {
		t.Run(test.name, func(t *testing.T) {
			s := testInfrastructure(t)
			var mu sync.Mutex
			var commands []string
			newDeviceMessagePeer(t, s, func(req *sip.Request, tx sip.ServerTransaction, _ *deviceQueryPeer) {
				var control ptzControl
				if err := xml.Unmarshal(req.Body(), &control); err != nil {
					t.Error(err)
					return
				}
				mu.Lock()
				commands = append(commands, control.PTZCommand)
				mu.Unlock()
				status := test.presetStatus
				if control.PTZCommand == "A50F0000000000B4" {
					status = test.stopStatus
				}
				if status != 0 {
					if err := tx.Respond(sip.NewResponseFromRequest(req, status, "test", nil)); err != nil {
						t.Error(err)
					}
				}
			})
			addPresetQueryChannels(t, s, presetQueryChannelID)
			response := apiRequest(t, s.handler(), "POST", "/api/devices/"+queryTestDeviceID+"/channels/"+presetQueryChannelID+"/ptz", `{"command":"preset_goto","preset_id":1}`, 502)
			if !strings.Contains(response.Body.String(), test.reason) {
				t.Fatal(response.Body.String())
			}
			mu.Lock()
			defer mu.Unlock()
			if len(commands) < 2 || commands[len(commands)-1] != "A50F0000000000B4" {
				t.Fatalf("missing final STOP %v", commands)
			}
			if len(s.live.ptzOwners) != 0 {
				t.Fatal("owner retained")
			}
		})
	}
}

func TestPresetControlCancellation(t *testing.T) {
	for _, reason := range []string{"manual STOP", "HTTP cancel", "device unregister", "server shutdown"} {
		t.Run(reason, func(t *testing.T) {
			s := testInfrastructure(t)
			received, stopped, release := make(chan struct{}), make(chan struct{}), make(chan struct{})
			var receiveOnce, stopOnce, releaseOnce sync.Once
			defer releaseOnce.Do(func() { close(release) })
			peer := newDeviceMessagePeer(t, s, func(req *sip.Request, tx sip.ServerTransaction, _ *deviceQueryPeer) {
				var control ptzControl
				if err := xml.Unmarshal(req.Body(), &control); err != nil {
					t.Error(err)
					return
				}
				if control.PTZCommand == "A50F0000000000B4" {
					stopOnce.Do(func() { close(stopped) })
					<-release
					if err := tx.Respond(sip.NewResponseFromRequest(req, 200, "OK", nil)); err != nil {
						t.Error(err)
					}
					return
				}
				receiveOnce.Do(func() { close(received) })
			})
			addPresetQueryChannels(t, s, presetQueryChannelID)
			path := "/api/devices/" + queryTestDeviceID + "/channels/" + presetQueryChannelID + "/ptz"
			ctx, cancel := context.WithCancel(t.Context())
			defer cancel()
			completed := make(chan *httptest.ResponseRecorder, 1)
			go func() {
				w := httptest.NewRecorder()
				r := httptest.NewRequest("POST", path, strings.NewReader(`{"command":"preset_goto","preset_id":1}`)).WithContext(ctx)
				r.Header.Set("Content-Type", "application/json")
				s.handler().ServeHTTP(w, r)
				completed <- w
			}()
			select {
			case <-received:
			case <-time.After(time.Second):
				t.Fatal("no preset")
			}
			switch reason {
			case "manual STOP":
				apiRequest(t, s.handler(), "POST", path, `{"command":"stop"}`, 202)
			case "HTTP cancel":
				cancel()
			case "device unregister":
				registration, _ := s.live.sip.devices.get(queryTestDeviceID)
				r := sip.NewRequest(sip.REGISTER, sip.Uri{Scheme: "sip", User: peer.platformID, Host: "127.0.0.1"})
				identity := sip.Uri{Scheme: "sip", User: queryTestDeviceID, Host: s.live.sip.cfg.sipDomain}
				r.AppendHeader(&sip.FromHeader{Address: identity, Params: sip.NewParams()})
				r.AppendHeader(&sip.ToHeader{Address: identity})
				r.AppendHeader(&sip.ContactHeader{Address: registration.contact})
				expires := sip.ExpiresHeader(0)
				r.AppendHeader(&expires)
				r.SetTransport("UDP")
				r.SetDestination(peer.platform)
				regContext, stop := context.WithTimeout(t.Context(), time.Second)
				defer stop()
				challenge, err := peer.client.Do(regContext, r)
				if err != nil {
					t.Fatal(err)
				}
				response, err := peer.client.DoDigestAuth(regContext, r, challenge, sipgo.DigestAuth{Username: queryTestDeviceID, Password: s.live.sip.cfg.sipPassword})
				if err != nil {
					t.Fatal(err)
				}
				if response.StatusCode != 200 {
					t.Fatalf("unregister SIP%d", response.StatusCode)
				}
			case "server shutdown":
				listener, err := net.Listen("tcp", "127.0.0.1:0")
				if err != nil {
					t.Fatal(err)
				}
				s.httpListen = listener.Addr().String()
				_ = listener.Close()
				serveContext, stopServe := context.WithCancel(t.Context())
				defer stopServe()
				done := make(chan error, 1)
				go func() { done <- s.serve(serveContext) }()
				client := &http.Client{Timeout: time.Second}
				deadline := time.Now().Add(time.Second)
				for {
					response, err := client.Get("http://" + s.httpListen + "/api/devices")
					if err == nil {
						_ = response.Body.Close()
						break
					}
					if time.Now().After(deadline) {
						t.Fatal("serve not ready")
					}
					time.Sleep(5 * time.Millisecond)
				}
				stopServe()
				select {
				case err := <-done:
					if err != nil {
						t.Fatal(err)
					}
				case <-time.After(time.Second):
					t.Fatal("serve did not stop")
				}
			}
			select {
			case <-stopped:
			case <-time.After(time.Second):
				t.Fatal("no independent STOP")
			}
			// 后续cancel只请求停止，不能取消独立STOP事务。
			apiRequest(t, s.handler(), "POST", path, `{"command":"stop"}`, 202)
			cancel()
			releaseOnce.Do(func() { close(release) })
			select {
			case w := <-completed:
				if w.Code != 502 || !strings.Contains(w.Body.String(), "preset_control_unconfirmed") {
					t.Fatalf("%d %s", w.Code, w.Body.String())
				}
			case <-time.After(time.Second):
				t.Fatal("owner did not complete")
			}
			s.live.mu.Lock()
			defer s.live.mu.Unlock()
			if len(s.live.ptzOwners) != 0 {
				t.Fatal("owner retained")
			}
		})
	}
}

func TestPresetControlHTTPDisconnect(t *testing.T) {
	s := testInfrastructure(t)
	received, stopped := make(chan struct{}), make(chan struct{})
	var once, stopOnce sync.Once
	newDeviceMessagePeer(t, s, func(req *sip.Request, tx sip.ServerTransaction, _ *deviceQueryPeer) {
		var control ptzControl
		if err := xml.Unmarshal(req.Body(), &control); err != nil {
			t.Error(err)
			return
		}
		if control.PTZCommand == "A50F0000000000B4" {
			stopOnce.Do(func() { close(stopped) })
			if err := tx.Respond(sip.NewResponseFromRequest(req, 200, "OK", nil)); err != nil {
				t.Error(err)
			}
		} else {
			once.Do(func() { close(received) })
		}
	})
	addPresetQueryChannels(t, s, presetQueryChannelID)
	server := httptest.NewServer(s.handler())
	defer server.Close()
	ctx, cancel := context.WithCancel(t.Context())
	defer cancel()
	request, err := http.NewRequestWithContext(ctx, "POST", server.URL+"/api/devices/"+queryTestDeviceID+"/channels/"+presetQueryChannelID+"/ptz", strings.NewReader(`{"command":"preset_goto","preset_id":1}`))
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
	case <-received:
	case <-time.After(time.Second):
		t.Fatal("no preset")
	}
	cancel()
	select {
	case <-stopped:
	case <-time.After(time.Second):
		t.Fatal("HTTP disconnect did not attempt STOP")
	}
	select {
	case err := <-done:
		if err == nil {
			t.Fatal("client cancellation unexpectedly succeeded")
		}
	case <-time.After(time.Second):
		t.Fatal("client retained")
	}
	server.Close()
	s.live.mu.Lock()
	defer s.live.mu.Unlock()
	if len(s.live.ptzOwners) != 0 {
		t.Fatal("owner retained")
	}
}

func TestPresetControlChannelConcurrency(t *testing.T) {
	s := testInfrastructure(t)
	received, release := make(chan string, 2), make(chan struct{})
	defer close(release)
	newDeviceMessagePeer(t, s, func(req *sip.Request, tx sip.ServerTransaction, _ *deviceQueryPeer) {
		var control ptzControl
		if err := xml.Unmarshal(req.Body(), &control); err != nil {
			t.Error(err)
			return
		}
		received <- control.DeviceID
		<-release
		if err := tx.Respond(sip.NewResponseFromRequest(req, 200, "OK", nil)); err != nil {
			t.Error(err)
		}
	})
	secondChannel := "34020000001320000003"
	addPresetQueryChannels(t, s, presetQueryChannelID, secondChannel)
	completed := make(chan *httptest.ResponseRecorder, 2)
	for _, channel := range []string{presetQueryChannelID, secondChannel} {
		go func() {
			w := httptest.NewRecorder()
			r := httptest.NewRequest("POST", "/api/devices/"+queryTestDeviceID+"/channels/"+channel+"/ptz", strings.NewReader(`{"command":"preset_set","preset_id":1}`))
			r.Header.Set("Content-Type", "application/json")
			s.handler().ServeHTTP(w, r)
			completed <- w
		}()
	}
	for range 2 {
		select {
		case <-received:
		case <-time.After(time.Second):
			t.Fatal("different channels serialized")
		}
	}
	s.live.mu.Lock()
	owners := len(s.live.ptzOwners)
	s.live.mu.Unlock()
	if owners != 2 {
		t.Fatalf("owners=%d", owners)
	}
	path := "/api/devices/" + queryTestDeviceID + "/channels/" + presetQueryChannelID + "/ptz"
	apiRequest(t, s.handler(), "POST", path, `{"command":"preset_remove","preset_id":1}`, 409)
	apiRequest(t, s.handler(), "POST", path, `{"command":"left","speed":64}`, 409)
	// 释放两项事务，各自成功移除自身owner，不删其他通道。
	for range 2 {
		release <- struct{}{}
	}
	for range 2 {
		select {
		case w := <-completed:
			if w.Code != 200 {
				t.Fatalf("%d %s", w.Code, w.Body.String())
			}
		case <-time.After(time.Second):
			t.Fatal("owner retained")
		}
	}
	s.live.mu.Lock()
	defer s.live.mu.Unlock()
	if len(s.live.ptzOwners) != 0 {
		t.Fatal("owners retained")
	}
}

func TestPresetControlLegacyResponse(t *testing.T) {
	s := testInfrastructure(t)
	newDeviceMessagePeer(t, s, func(req *sip.Request, tx sip.ServerTransaction, _ *deviceQueryPeer) {
		if err := tx.Respond(sip.NewResponseFromRequest(req, 200, "OK", nil)); err != nil {
			t.Error(err)
		}
	})
	addPresetQueryChannels(t, s, presetQueryChannelID)
	path := "/api/devices/" + queryTestDeviceID + "/channels/" + presetQueryChannelID + "/ptz"
	for _, test := range []struct{ body, want string }{
		{`{"command":"stop"}`, `{"device_id":"34020000001320000001","channel_id":"34020000001320000002","command":"stop","stop":"accepted","physical_result":"unconfirmed"}` + "\n"},
		{`{"command":"stop","speed":null}`, `{"device_id":"34020000001320000001","channel_id":"34020000001320000002","command":"stop","stop":"accepted","physical_result":"unconfirmed"}` + "\n"},
		{`{"command":"left","speed":64}`, `{"device_id":"34020000001320000001","channel_id":"34020000001320000002","command":"left","start":"accepted","stop":"accepted","physical_result":"unconfirmed"}` + "\n"},
	} {
		response := apiRequest(t, s.handler(), "POST", path, test.body, 200)
		if response.Body.String() != test.want {
			t.Fatalf("legacy changed: %s", response.Body.String())
		}
	}
}

func TestPresetControlSuccessStopRace(t *testing.T) {
	// 两个合法线性化结果：已有owner返回202由它发STOP，或成功释放后独立STOP返回200。
	for attempt := 0; attempt < 12; attempt++ {
		t.Run(fmt.Sprint(attempt), func(t *testing.T) {
			s := testInfrastructure(t)
			received, release := make(chan struct{}), make(chan struct{})
			callbacksDone := make(chan struct{}, 2)
			var once sync.Once
			var mu sync.Mutex
			stops := 0
			newDeviceMessagePeer(t, s, func(req *sip.Request, tx sip.ServerTransaction, _ *deviceQueryPeer) {
				defer func() { callbacksDone <- struct{}{} }()
				var control ptzControl
				if err := xml.Unmarshal(req.Body(), &control); err != nil {
					t.Error(err)
					return
				}
				if control.PTZCommand == "A50F0000000000B4" {
					mu.Lock()
					stops++
					mu.Unlock()
				} else {
					once.Do(func() { close(received) })
					<-release
				}
				if err := tx.Respond(sip.NewResponseFromRequest(req, 200, "OK", nil)); err != nil {
					t.Error(err)
				}
			})
			addPresetQueryChannels(t, s, presetQueryChannelID)
			path := "/api/devices/" + queryTestDeviceID + "/channels/" + presetQueryChannelID + "/ptz"
			presetDone, stopDone := make(chan *httptest.ResponseRecorder, 1), make(chan *httptest.ResponseRecorder, 1)
			go func() {
				w := httptest.NewRecorder()
				r := httptest.NewRequest("POST", path, strings.NewReader(`{"command":"preset_goto","preset_id":1}`))
				r.Header.Set("Content-Type", "application/json")
				s.handler().ServeHTTP(w, r)
				presetDone <- w
			}()
			select {
			case <-received:
			case <-time.After(time.Second):
				t.Fatal("no preset")
			}
			go func() {
				<-release
				w := httptest.NewRecorder()
				r := httptest.NewRequest("POST", path, strings.NewReader(`{"command":"stop"}`))
				r.Header.Set("Content-Type", "application/json")
				s.handler().ServeHTTP(w, r)
				stopDone <- w
			}()
			close(release)
			select {
			case w := <-stopDone:
				if w.Code != 200 && w.Code != 202 {
					t.Fatalf("STOP %d %s", w.Code, w.Body.String())
				}
				t.Logf("交错STOP HTTP%d", w.Code)
			case <-time.After(2 * time.Second):
				t.Fatal("STOP timeout")
			}
			select {
			case w := <-presetDone:
				if w.Code != 200 && w.Code != 502 {
					t.Fatalf("preset %d %s", w.Code, w.Body.String())
				}
				t.Logf("交错preset HTTP%d", w.Code)
			case <-time.After(2 * time.Second):
				t.Fatal("preset timeout")
			}
			// HTTP收到响应不代表peer的同步Respond已返回；先收尾两个回调再关闭fixture socket。
			for range 2 {
				select {
				case <-callbacksDone:
				case <-time.After(time.Second):
					t.Fatal("UDP callback retained")
				}
			}
			mu.Lock()
			defer mu.Unlock()
			if stops != 1 {
				t.Fatalf("STOP requests=%d", stops)
			}
			s.live.mu.Lock()
			defer s.live.mu.Unlock()
			if len(s.live.ptzOwners) != 0 {
				t.Fatal("owner retained")
			}
		})
	}
}
