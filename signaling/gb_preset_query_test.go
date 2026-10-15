package main

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/emiago/sipgo"
	"github.com/emiago/sipgo/sip"
	"golang.org/x/text/encoding/simplifiedchinese"
)

const presetQueryChannelID = "34020000001320000002"

func addPresetQueryChannels(t *testing.T, s *infrastructureServer, channelIDs ...string) {
	t.Helper()
	s.live.sip.channels.beginQuery(queryTestDeviceID, 1)
	var response catalogResponse
	response.DeviceID, response.SN, response.SumNum, response.DeviceList.Num = queryTestDeviceID, 1, len(channelIDs), len(channelIDs)
	for _, id := range channelIDs {
		response.DeviceList.Items = append(response.DeviceList.Items, catalogChannel{DeviceID: id, Name: "test channel", Status: "ON"})
	}
	if err := s.live.sip.channels.apply(response); err != nil {
		t.Fatal(err)
	}
}

func presetResponseXML(query manscdpQuery, details string) []byte {
	return []byte(fmt.Sprintf("<Response><CmdType>%s</CmdType><SN>%d</SN><DeviceID>%s</DeviceID>%s</Response>", query.CmdType, query.SN, query.DeviceID, details))
}

func TestPresetQueryHTTPPreconditions(t *testing.T) {
	for _, path := range []string{
		"/api/devices/invalid/channels/" + presetQueryChannelID + "/presets/query",
		"/api/devices/" + queryTestDeviceID + "/channels/invalid/presets/query",
	} {
		s := testInfrastructure(t)
		apiRequest(t, s.handler(), "POST", path, "", http.StatusBadRequest)
	}
	for _, test := range []struct {
		name                              string
		create, channel, online, stopping bool
		status                            int
	}{
		{name: "unknown", status: http.StatusNotFound},
		{name: "unknown channel", create: true, online: true, status: http.StatusNotFound},
		{name: "offline", create: true, channel: true, status: http.StatusConflict},
		{name: "stopping", create: true, channel: true, online: true, stopping: true, status: http.StatusConflict},
	} {
		t.Run(test.name, func(t *testing.T) {
			s := testInfrastructure(t)
			if test.create {
				if err := s.live.sip.deviceStore.create(t.Context(), gbDevice{deviceID: queryTestDeviceID, name: "test"}); err != nil {
					t.Fatal(err)
				}
			}
			if test.channel {
				addPresetQueryChannels(t, s, presetQueryChannelID)
			}
			if test.online {
				s.live.sip.devices.register(registeredDevice{id: queryTestDeviceID, online: true, expiresAt: time.Now().Add(time.Hour)})
			}
			if test.stopping {
				s.live.stoppingDevices[queryTestDeviceID] = struct{}{}
			}
			apiRequest(t, s.handler(), "POST", "/api/devices/"+queryTestDeviceID+"/channels/"+presetQueryChannelID+"/presets/query", "", test.status)
			if s.live.sip.messageSN.Load() != 0 || len(s.live.sip.pendingQueries) != 0 {
				t.Fatal("precondition allocated query")
			}
		})
	}
}

func TestPresetQueryOfflineAfterChannelCleanup(t *testing.T) {
	for _, reason := range []string{"unregister", "registration expired", "heartbeat expired"} {
		t.Run(reason, func(t *testing.T) {
			s := testInfrastructure(t)
			peer := newDeviceQueryPeer(t, s, func(manscdpQuery, *sip.Request, sip.ServerTransaction, *deviceQueryPeer) {
				t.Error("offline query sent SIP MESSAGE")
			})
			addPresetQueryChannels(t, s, presetQueryChannelID)
			now := s.live.sip.now()
			device, _ := s.live.sip.devices.get(queryTestDeviceID)
			device.lastHeartbeat = now
			s.live.sip.devices.register(device)
			if _, online := s.live.sip.devices.getOnline(queryTestDeviceID, now); !online {
				t.Fatal("device was not online before cleanup")
			}
			if _, exists := s.live.sip.channels.get(queryTestDeviceID, presetQueryChannelID); !exists {
				t.Fatal("Catalog channel missing before cleanup")
			}
			if reason == "unregister" {
				request := sip.NewRequest(sip.REGISTER, sip.Uri{Scheme: "sip", User: peer.platformID, Host: "127.0.0.1"})
				identity := sip.Uri{Scheme: "sip", User: queryTestDeviceID, Host: s.live.sip.cfg.sipDomain}
				params := sip.NewParams()
				params.Add("tag", sip.GenerateTagN(16))
				request.AppendHeader(&sip.FromHeader{Address: identity, Params: params})
				request.AppendHeader(&sip.ToHeader{Address: identity})
				request.AppendHeader(&sip.ContactHeader{Address: device.contact})
				expires := sip.ExpiresHeader(0)
				request.AppendHeader(&expires)
				request.SetTransport("UDP")
				request.SetDestination(peer.platform)
				ctx, cancel := context.WithTimeout(t.Context(), time.Second)
				defer cancel()
				challenge, err := peer.client.Do(ctx, request)
				if err != nil {
					t.Fatal(err)
				}
				if challenge.StatusCode != sip.StatusUnauthorized {
					t.Fatalf("unregister challenge: SIP %d", challenge.StatusCode)
				}
				response, err := peer.client.DoDigestAuth(ctx, request, challenge, sipgo.DigestAuth{Username: queryTestDeviceID, Password: s.live.sip.cfg.sipPassword})
				if err != nil {
					t.Fatal(err)
				}
				if response.StatusCode != sip.StatusOK {
					t.Fatalf("unregister: SIP %d", response.StatusCode)
				}
			} else if reason == "registration expired" {
				s.live.sip.expireDevices(device.expiresAt)
			} else {
				s.live.sip.expireDevices(now.Add(s.live.sip.cfg.heartbeatTimeout))
			}
			if _, online := s.live.sip.devices.getOnline(queryTestDeviceID, s.live.sip.now()); online {
				t.Fatal("device remained online after cleanup")
			}
			if _, exists := s.live.sip.channels.get(queryTestDeviceID, presetQueryChannelID); exists {
				t.Fatal("offline lifecycle did not remove channel")
			}
			response := apiRequest(t, s.handler(), "POST", "/api/devices/"+queryTestDeviceID+"/channels/"+presetQueryChannelID+"/presets/query", "", http.StatusConflict)
			if !strings.Contains(response.Body.String(), `"error":"device_offline"`) {
				t.Fatalf("offline response: %s", response.Body.String())
			}
			if s.live.sip.messageSN.Load() != 0 || len(s.live.sip.pendingQueries) != 0 {
				t.Fatal("offline query allocated request")
			}
		})
	}
}

func TestPresetQueryHTTPResults(t *testing.T) {
	for _, test := range []struct {
		name, details string
		gbk           bool
	}{
		{name: "empty", details: `<PresetList Num="0"/>`},
		{name: "string IDs and empty name", details: `<PresetList Num="2"><Item><PresetID>camera-A</PresetID><PresetName>北门</PresetName></Item><Item><PresetID>1000</PresetID><PresetName></PresetName></Item></PresetList>`},
		{name: "GB2312", details: `<PresetList Num="1"><Item><PresetID>中文编号</PresetID><PresetName>正门</PresetName></Item></PresetList>`, gbk: true},
	} {
		t.Run(test.name, func(t *testing.T) {
			s := testInfrastructure(t)
			peer := newDeviceQueryPeer(t, s, func(query manscdpQuery, request *sip.Request, tx sip.ServerTransaction, peer *deviceQueryPeer) {
				if query.DeviceID != presetQueryChannelID || query.CmdType != "PresetQuery" || request.To().Address.User != queryTestDeviceID {
					t.Errorf("query recipient/target: %+v", query)
				}
				body := presetResponseXML(query, test.details)
				if test.gbk {
					var err error
					body, err = simplifiedchinese.GBK.NewEncoder().Bytes(append([]byte(`<?xml version="1.0" encoding="GB2312"?>`), body...))
					if err != nil {
						t.Error(err)
						return
					}
				}
				peer.send(t, body, queryTestDeviceID, sip.StatusOK)
				if err := tx.Respond(sip.NewResponseFromRequest(request, sip.StatusOK, "OK", nil)); err != nil {
					t.Error(err)
				}
			})
			addPresetQueryChannels(t, s, presetQueryChannelID)
			response := apiRequest(t, s.handler(), "POST", "/api/devices/"+queryTestDeviceID+"/channels/"+presetQueryChannelID+"/presets/query", "", http.StatusOK)
			var result deviceQueryResponse
			if err := json.Unmarshal(response.Body.Bytes(), &result); err != nil {
				t.Fatal(err)
			}
			if result.DeviceID != queryTestDeviceID || result.ChannelID != presetQueryChannelID || result.Command != "PresetQuery" || result.SN <= 0 || result.Result != "" || result.Presets == nil || result.Info != nil || result.Status != nil {
				t.Fatalf("result: %s", response.Body.String())
			}
			if strings.Contains(response.Body.String(), `"result"`) || strings.Contains(response.Body.String(), `"presets":null`) {
				t.Fatalf("non-2016 result: %s", response.Body.String())
			}
			if test.name == "empty" && len(*result.Presets) != 0 {
				t.Fatalf("empty: %s", response.Body.String())
			}
			if test.gbk && ((*result.Presets)[0].ID != "中文编号" || (*result.Presets)[0].Name != "正门") {
				t.Fatalf("GB2312: %s", response.Body.String())
			}
			if test.name == "string IDs and empty name" && (len(*result.Presets) != 2 || (*result.Presets)[0].ID != "camera-A" || (*result.Presets)[1].ID != "1000" || (*result.Presets)[1].Name != "") {
				t.Fatalf("string IDs: %s", response.Body.String())
			}
			s.live.mu.Lock()
			pending := len(s.live.sip.pendingQueries)
			s.live.mu.Unlock()
			if pending != 0 {
				t.Fatal("completed query retained")
			}
			peer.send(t, presetResponseXML(manscdpQuery{CmdType: "PresetQuery", SN: result.SN, DeviceID: presetQueryChannelID}, test.details), queryTestDeviceID, sip.StatusOK)
		})
	}
}

func TestPresetQuerySchemaAndCorrelation(t *testing.T) {
	s := testInfrastructure(t)
	newDeviceQueryPeer(t, s, func(query manscdpQuery, request *sip.Request, tx sip.ServerTransaction, peer *deviceQueryPeer) {
		for _, details := range []string{
			"", `<PresetList/>`, `<PresetList Num="-1"/>`, `<PresetList Num="1"/>`,
			`<PresetList Num="1"><Item><PresetID></PresetID></Item></PresetList>`,
			`<PresetList Num="2"><Item><PresetID>A</PresetID></Item><Item><PresetID>A</PresetID></Item></PresetList>`,
		} {
			peer.send(t, presetResponseXML(query, details), queryTestDeviceID, sip.StatusBadRequest)
		}
		valid := `<PresetList Num="1"><Item><PresetID>first</PresetID><PresetName/></Item></PresetList>`
		wrong := query
		wrong.SN++
		peer.send(t, presetResponseXML(wrong, valid), queryTestDeviceID, sip.StatusOK)
		wrong = query
		wrong.DeviceID = "34020000001320000003"
		peer.send(t, presetResponseXML(wrong, valid), queryTestDeviceID, sip.StatusOK)
		peer.send(t, presetResponseXML(query, valid), presetQueryChannelID, sip.StatusForbidden)
		registration, _ := s.live.sip.devices.get(queryTestDeviceID)
		other := registration
		other.id = "34020000001320000004"
		s.live.sip.devices.register(other)
		peer.send(t, presetResponseXML(query, valid), other.id, sip.StatusForbidden)
		s.live.sip.devices.unregister(other.id)
		changed := registration
		changed.remoteEndpoint = "127.0.0.1:1"
		s.live.sip.devices.register(changed)
		peer.send(t, presetResponseXML(query, valid), queryTestDeviceID, sip.StatusForbidden)
		s.live.sip.devices.register(registration)
		s.live.mu.Lock()
		s.live.stoppingDevices[queryTestDeviceID] = struct{}{}
		s.live.mu.Unlock()
		peer.send(t, presetResponseXML(query, valid), queryTestDeviceID, sip.StatusForbidden)
		s.live.mu.Lock()
		delete(s.live.stoppingDevices, queryTestDeviceID)
		s.live.mu.Unlock()
		wrong = query
		wrong.CmdType = "DeviceInfo"
		peer.send(t, queryResponseXML(wrong, "OK", ""), queryTestDeviceID, sip.StatusBadRequest)
		peer.send(t, presetResponseXML(query, valid), queryTestDeviceID, sip.StatusOK)
		peer.send(t, presetResponseXML(query, strings.ReplaceAll(valid, "first", "duplicate")), queryTestDeviceID, sip.StatusOK)
		if err := tx.Respond(sip.NewResponseFromRequest(request, sip.StatusOK, "OK", nil)); err != nil {
			t.Error(err)
		}
	})
	addPresetQueryChannels(t, s, presetQueryChannelID)
	response := apiRequest(t, s.handler(), "POST", "/api/devices/"+queryTestDeviceID+"/channels/"+presetQueryChannelID+"/presets/query", "", http.StatusOK)
	var result deviceQueryResponse
	if err := json.Unmarshal(response.Body.Bytes(), &result); err != nil {
		t.Fatal(err)
	}
	if result.Presets == nil || (*result.Presets)[0].ID != "first" {
		t.Fatalf("wrong response accepted: %s", response.Body.String())
	}
}

func TestPresetQueryDeadlineAndRejection(t *testing.T) {
	for _, test := range []struct {
		name          string
		sipStatus     int
		early, cancel bool
		expected      int
	}{
		{name: "SIP 200 is not presets", sipStatus: sip.StatusOK, expected: http.StatusGatewayTimeout},
		{name: "reject", sipStatus: sip.StatusForbidden, expected: http.StatusBadGateway},
		{name: "reject after early XML", sipStatus: sip.StatusForbidden, early: true, expected: http.StatusBadGateway},
		{name: "cancel", cancel: true, expected: http.StatusBadGateway},
	} {
		t.Run(test.name, func(t *testing.T) {
			s := testInfrastructure(t)
			ctx, cancel := context.WithTimeout(t.Context(), 250*time.Millisecond)
			defer cancel()
			newDeviceQueryPeer(t, s, func(query manscdpQuery, request *sip.Request, tx sip.ServerTransaction, peer *deviceQueryPeer) {
				if test.early {
					peer.send(t, presetResponseXML(query, `<PresetList Num="0"/>`), queryTestDeviceID, sip.StatusOK)
				}
				if test.cancel {
					cancel()
				}
				if test.sipStatus != 0 {
					if err := tx.Respond(sip.NewResponseFromRequest(request, test.sipStatus, "Test", nil)); err != nil {
						t.Error(err)
					}
				}
			})
			addPresetQueryChannels(t, s, presetQueryChannelID)
			request := httptest.NewRequest("POST", "/api/devices/"+queryTestDeviceID+"/channels/"+presetQueryChannelID+"/presets/query", nil).WithContext(ctx)
			response := httptest.NewRecorder()
			s.handler().ServeHTTP(response, request)
			if response.Code != test.expected {
				t.Fatalf("HTTP=%d want=%d: %s", response.Code, test.expected, response.Body.String())
			}
			s.live.mu.Lock()
			pending := len(s.live.sip.pendingQueries)
			s.live.mu.Unlock()
			if pending != 0 {
				t.Fatal("failed query retained")
			}
		})
	}
}

func TestPresetQueryConcurrentChannelsAndDeviceInfo(t *testing.T) {
	s := testInfrastructure(t)
	queries := make(chan manscdpQuery, 3)
	peer := newDeviceQueryPeer(t, s, func(query manscdpQuery, request *sip.Request, tx sip.ServerTransaction, _ *deviceQueryPeer) {
		queries <- query
		if err := tx.Respond(sip.NewResponseFromRequest(request, sip.StatusOK, "OK", nil)); err != nil {
			t.Error(err)
		}
	})
	addPresetQueryChannels(t, s, presetQueryChannelID, "34020000001320000003")
	var wait sync.WaitGroup
	for _, channelID := range []string{presetQueryChannelID, "34020000001320000003", ""} {
		wait.Add(1)
		go func() {
			defer wait.Done()
			path, body := "/api/devices/"+queryTestDeviceID+"/channels/"+channelID+"/presets/query", ""
			if channelID == "" {
				path, body = "/api/devices/"+queryTestDeviceID+"/query", `{"command":"DeviceInfo"}`
			}
			response := apiRequest(t, s.handler(), "POST", path, body, http.StatusOK)
			var result deviceQueryResponse
			if err := json.Unmarshal(response.Body.Bytes(), &result); err != nil {
				t.Error(err)
				return
			}
			if result.ChannelID != channelID || result.DeviceID != queryTestDeviceID {
				t.Errorf("mixed target: %s", response.Body.String())
			}
			if channelID == "" && (result.Info == nil || result.Presets != nil || result.Result != "OK") {
				t.Errorf("mixed info: %s", response.Body.String())
			}
			if channelID != "" && (result.Presets == nil || (*result.Presets)[0].ID != channelID || result.Info != nil) {
				t.Errorf("mixed presets: %s", response.Body.String())
			}
		}()
	}
	seen := make(map[int]bool)
	for i := 0; i < 3; i++ {
		select {
		case query := <-queries:
			if seen[query.SN] {
				t.Error("reused SN")
			}
			seen[query.SN] = true
			if query.CmdType == "DeviceInfo" {
				peer.send(t, queryResponseXML(query, "OK", ""), queryTestDeviceID, sip.StatusOK)
			} else {
				wrong := query
				wrong.DeviceID = queryTestDeviceID
				peer.send(t, presetResponseXML(wrong, `<PresetList Num="0"/>`), queryTestDeviceID, sip.StatusOK)
				peer.send(t, presetResponseXML(query, `<PresetList Num="1"><Item><PresetID>`+query.DeviceID+`</PresetID></Item></PresetList>`), queryTestDeviceID, sip.StatusOK)
			}
		case <-time.After(time.Second):
			t.Error("missing concurrent query")
		}
	}
	wait.Wait()
	s.live.mu.Lock()
	defer s.live.mu.Unlock()
	if len(s.live.sip.pendingQueries) != 0 {
		t.Fatal("pending queries retained")
	}
}

func TestPresetQueryTotalDeadline(t *testing.T) {
	s := testInfrastructure(t)
	newDeviceQueryPeer(t, s, func(_ manscdpQuery, request *sip.Request, tx sip.ServerTransaction, _ *deviceQueryPeer) {
		if err := tx.Respond(sip.NewResponseFromRequest(request, sip.StatusOK, "OK", nil)); err != nil {
			t.Error(err)
		}
	})
	addPresetQueryChannels(t, s, presetQueryChannelID)
	ctx, cancel := context.WithTimeout(t.Context(), 8*time.Second)
	defer cancel()
	request := httptest.NewRequest("POST", "/api/devices/"+queryTestDeviceID+"/channels/"+presetQueryChannelID+"/presets/query", nil).WithContext(ctx)
	response := httptest.NewRecorder()
	start := time.Now()
	s.handler().ServeHTTP(response, request)
	if response.Code != http.StatusGatewayTimeout || ctx.Err() != nil || time.Since(start) < 4*time.Second || time.Since(start) > 7*time.Second {
		t.Fatalf("total deadline HTTP=%d elapsed=%s outer=%v", response.Code, time.Since(start), ctx.Err())
	}
	s.live.mu.Lock()
	defer s.live.mu.Unlock()
	if len(s.live.sip.pendingQueries) != 0 {
		t.Fatal("timed out query retained")
	}
}
