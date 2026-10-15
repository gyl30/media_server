package main

import (
	"context"
	"encoding/json"
	"encoding/xml"
	"fmt"
	"net"
	"net/http"
	"net/http/httptest"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/emiago/sipgo"
	"github.com/emiago/sipgo/sip"
	"golang.org/x/text/encoding/simplifiedchinese"
)

func TestDeviceQueryHTTPPreconditions(t *testing.T) {
	for _, body := range []string{`{}`, `{"command":"PTZ"}`, `{"command":"DeviceInfo","extra":1}`} {
		s := testInfrastructure(t)
		apiRequest(t, s.handler(), "POST", "/api/devices/34020000001320000001/query", body, http.StatusBadRequest)
	}
	for _, test := range []struct {
		name                     string
		create, online, stopping bool
		status                   int
	}{
		{name: "unknown", status: http.StatusNotFound},
		{name: "offline", create: true, status: http.StatusConflict},
		{name: "stopping", create: true, online: true, stopping: true, status: http.StatusConflict},
	} {
		t.Run(test.name, func(t *testing.T) {
			s := testInfrastructure(t)
			if test.create {
				if err := s.live.sip.deviceStore.create(t.Context(), gbDevice{deviceID: queryTestDeviceID, name: "test"}); err != nil {
					t.Fatal(err)
				}
			}
			if test.online {
				s.live.sip.devices.register(registeredDevice{id: queryTestDeviceID, online: true, expiresAt: time.Now().Add(time.Hour)})
			}
			if test.stopping {
				s.live.stoppingDevices[queryTestDeviceID] = struct{}{}
			}
			apiRequest(t, s.handler(), "POST", "/api/devices/"+queryTestDeviceID+"/query", `{"command":"DeviceInfo"}`, test.status)
			if s.live.sip.messageSN.Load() != 0 || len(s.live.sip.pendingQueries) != 0 {
				t.Fatal("precondition failure allocated query")
			}
		})
	}
}

const queryTestDeviceID = "34020000001320000001"

// 测试端使用真实 UDP MESSAGE 与响应；它不是设备功能或硬件验收。
type deviceQueryPeer struct {
	client     *sipgo.Client
	platform   string
	platformID string
}

func newDeviceQueryPeer(t *testing.T, infrastructure *infrastructureServer, handler func(manscdpQuery, *sip.Request, sip.ServerTransaction, *deviceQueryPeer)) *deviceQueryPeer {
	t.Helper()
	return newDeviceMessagePeer(t, infrastructure, func(request *sip.Request, transaction sip.ServerTransaction, peer *deviceQueryPeer) {
		var query manscdpQuery
		if err := xml.Unmarshal(request.Body(), &query); err != nil {
			t.Error(err)
			return
		}
		handler(query, request, transaction, peer)
	})
}

func newDeviceMessagePeer(t *testing.T, infrastructure *infrastructureServer, handler func(*sip.Request, sip.ServerTransaction, *deviceQueryPeer)) *deviceQueryPeer {
	t.Helper()
	platformListener, err := net.ListenPacket("udp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	platformDone := make(chan error, 1)
	go func() { platformDone <- infrastructure.live.sip.server.ServeUDP(platformListener) }()
	t.Cleanup(func() { _ = platformListener.Close(); <-platformDone })
	platformClient, err := sipgo.NewClient(infrastructure.live.sip.ua, sipgo.WithClientAddr(platformListener.LocalAddr().String()), sipgo.WithClientConnectionAddr(platformListener.LocalAddr().String()))
	if err != nil {
		t.Fatal(err)
	}
	infrastructure.live.sip.client = platformClient
	listener, err := net.ListenPacket("udp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	ua, err := sipgo.NewUA()
	if err != nil {
		_ = listener.Close()
		t.Fatal(err)
	}
	server, err := sipgo.NewServer(ua)
	if err != nil {
		_ = listener.Close()
		_ = ua.Close()
		t.Fatal(err)
	}
	client, err := sipgo.NewClient(ua, sipgo.WithClientAddr(listener.LocalAddr().String()), sipgo.WithClientConnectionAddr(listener.LocalAddr().String()))
	if err != nil {
		_ = listener.Close()
		_ = ua.Close()
		t.Fatal(err)
	}
	peer := &deviceQueryPeer{client: client, platform: platformListener.LocalAddr().String(), platformID: infrastructure.live.sip.cfg.sipID}
	server.OnMessage(func(request *sip.Request, transaction sip.ServerTransaction) {
		handler(request, transaction, peer)
	})
	done := make(chan error, 1)
	go func() { done <- server.ServeUDP(listener) }()
	t.Cleanup(func() { _ = listener.Close(); <-done; _ = ua.Close() })
	if err := infrastructure.live.sip.deviceStore.create(t.Context(), gbDevice{deviceID: queryTestDeviceID, name: "query test"}); err != nil {
		t.Fatal(err)
	}
	host, portText, _ := net.SplitHostPort(listener.LocalAddr().String())
	port, _ := strconv.Atoi(portText)
	infrastructure.live.sip.devices.register(registeredDevice{id: queryTestDeviceID, online: true, expiresAt: time.Now().Add(time.Hour), remoteEndpoint: listener.LocalAddr().String(), contact: sip.Uri{Scheme: "sip", User: queryTestDeviceID, Host: host, Port: port}})
	return peer
}

func (peer *deviceQueryPeer) send(t *testing.T, body []byte, fromID string, expected int) {
	t.Helper()
	request := sip.NewRequest(sip.MESSAGE, sip.Uri{Scheme: "sip", User: peer.platformID, Host: "127.0.0.1"})
	request.AppendHeader(&sip.FromHeader{Address: sip.Uri{Scheme: "sip", User: fromID, Host: "127.0.0.1"}, Params: sip.NewParams()})
	request.AppendHeader(&sip.ToHeader{Address: request.Recipient})
	request.AppendHeader(sip.NewHeader("Content-Type", "Application/MANSCDP+xml"))
	request.SetBody(body)
	request.SetTransport("UDP")
	request.SetDestination(peer.platform)
	ctx, cancel := context.WithTimeout(t.Context(), time.Second)
	defer cancel()
	response, err := peer.client.Do(ctx, request)
	if err != nil {
		t.Error(err)
		return
	}
	if response.StatusCode != expected {
		t.Errorf("response SIP %d want %d", response.StatusCode, expected)
	}
}

func queryResponseXML(query manscdpQuery, result, details string) []byte {
	return []byte(fmt.Sprintf("<Response><CmdType>%s</CmdType><SN>%d</SN><DeviceID>%s</DeviceID><Result>%s</Result>%s</Response>", query.CmdType, query.SN, query.DeviceID, result, details))
}

func TestDeviceQueryHTTPResults(t *testing.T) {
	for _, test := range []struct {
		name, command, result, details string
		gbk                            bool
	}{
		{name: "info", command: "DeviceInfo", result: "OK", details: "<DeviceName>测试摄像机</DeviceName><Manufacturer>maker</Manufacturer><Model>model</Model><Firmware>1.0</Firmware><Channel>0</Channel>"},
		{name: "info channel omitted", command: "DeviceInfo", result: "OK"},
		{name: "GBK", command: "DeviceInfo", result: "OK", details: "<DeviceName>中文设备</DeviceName>", gbk: true},
		{name: "status", command: "DeviceStatus", result: "OK", details: "<Online>OFFLINE</Online><Status>ERROR</Status><DeviceTime>2026-10-15T12:34:56</DeviceTime><Encode>ON</Encode><Record>OFF</Record><Reason>test</Reason>"},
		{name: "business error", command: "DeviceStatus", result: "ERROR"},
	} {
		t.Run(test.name, func(t *testing.T) {
			s := testInfrastructure(t)
			peer := newDeviceQueryPeer(t, s, func(query manscdpQuery, request *sip.Request, tx sip.ServerTransaction, peer *deviceQueryPeer) {
				body := queryResponseXML(query, test.result, test.details)
				if test.gbk {
					var err error
					body, err = simplifiedchinese.GBK.NewEncoder().Bytes(append([]byte(`<?xml version="1.0" encoding="GB2312"?>`), body...))
					if err != nil {
						t.Error(err)
						return
					}
				}
				// 业务响应先于 Query 的 SIP 200 也必须正确关联。
				peer.send(t, body, query.DeviceID, sip.StatusOK)
				if err := tx.Respond(sip.NewResponseFromRequest(request, sip.StatusOK, "OK", nil)); err != nil {
					t.Error(err)
				}
			})
			response := apiRequest(t, s.handler(), "POST", "/api/devices/"+queryTestDeviceID+"/query", `{"command":"`+test.command+`"}`, http.StatusOK)
			var result deviceQueryResponse
			if err := json.Unmarshal(response.Body.Bytes(), &result); err != nil {
				t.Fatal(err)
			}
			if result.Command != test.command || result.Result != test.result || result.DeviceID != queryTestDeviceID || result.SN <= 0 {
				t.Fatalf("result: %+v", result)
			}
			if test.command == "DeviceInfo" {
				if result.Info == nil || result.Status != nil {
					t.Fatalf("result: %+v", result)
				}
				if test.name == "info" && (result.Info.Channel == nil || *result.Info.Channel != 0 || result.Info.DeviceName != "测试摄像机") {
					t.Fatalf("info: %+v", result.Info)
				}
				if test.name == "info channel omitted" && result.Info.Channel != nil {
					t.Fatal("omitted Channel became zero")
				}
				if test.gbk && result.Info.DeviceName != "中文设备" {
					t.Fatalf("GBK name %q", result.Info.DeviceName)
				}
			} else if result.Status == nil || result.Info != nil {
				t.Fatalf("result: %+v", result)
			}
			s.live.mu.Lock()
			pending := len(s.live.sip.pendingQueries)
			s.live.mu.Unlock()
			if pending != 0 {
				t.Fatal("pending query retained")
			}
			if _, online := s.live.sip.devices.getOnline(queryTestDeviceID, time.Now()); !online {
				t.Fatal("reported DeviceStatus changed registration")
			}
			// 请求结束后迟到响应仅受理并丢弃，不能保留结果或重建 pending。
			peer.send(t, queryResponseXML(manscdpQuery{CmdType: test.command, SN: result.SN, DeviceID: queryTestDeviceID}, test.result, test.details), queryTestDeviceID, sip.StatusOK)
		})
	}
}

func TestDeviceQueryResponseCorrelation(t *testing.T) {
	s := testInfrastructure(t)
	newDeviceQueryPeer(t, s, func(query manscdpQuery, request *sip.Request, tx sip.ServerTransaction, peer *deviceQueryPeer) {
		wrong := query
		wrong.SN++
		peer.send(t, queryResponseXML(wrong, "OK", ""), query.DeviceID, sip.StatusOK)
		wrong = query
		wrong.CmdType = "DeviceStatus"
		peer.send(t, queryResponseXML(wrong, "OK", "<Online>ONLINE</Online><Status>OK</Status>"), query.DeviceID, sip.StatusOK)
		peer.send(t, queryResponseXML(query, "OK", ""), "34020000001320000002", sip.StatusBadRequest)
		wrong = query
		wrong.DeviceID = "34020000001320000002"
		peer.send(t, queryResponseXML(wrong, "OK", ""), wrong.DeviceID, sip.StatusForbidden)
		other, _ := s.live.sip.devices.get(query.DeviceID)
		other.id = wrong.DeviceID
		s.live.sip.devices.register(other)
		peer.send(t, queryResponseXML(wrong, "OK", ""), wrong.DeviceID, sip.StatusOK)
		s.live.sip.devices.unregister(other.id)
		// 同设备重新注册后的当前端点才可信；不把最初发请求的 endpoint 当永久权限。
		registration, _ := s.live.sip.devices.get(query.DeviceID)
		changed := registration
		changed.remoteEndpoint = "127.0.0.1:1"
		s.live.sip.devices.register(changed)
		peer.send(t, queryResponseXML(query, "OK", ""), query.DeviceID, sip.StatusForbidden)
		s.live.sip.devices.register(registration)
		s.live.mu.Lock()
		s.live.stoppingDevices[query.DeviceID] = struct{}{}
		s.live.mu.Unlock()
		peer.send(t, queryResponseXML(query, "OK", ""), query.DeviceID, sip.StatusForbidden)
		s.live.mu.Lock()
		delete(s.live.stoppingDevices, query.DeviceID)
		s.live.mu.Unlock()
		peer.send(t, queryResponseXML(query, "OK", "<Channel>-1</Channel>"), query.DeviceID, sip.StatusBadRequest)
		peer.send(t, queryResponseXML(query, "invalid", ""), query.DeviceID, sip.StatusBadRequest)
		peer.send(t, []byte("<Response>"), query.DeviceID, sip.StatusBadRequest)
		peer.send(t, queryResponseXML(query, "OK", "<DeviceName>first</DeviceName>"), query.DeviceID, sip.StatusOK)
		peer.send(t, queryResponseXML(query, "OK", "<DeviceName>duplicate</DeviceName>"), query.DeviceID, sip.StatusOK)
		if err := tx.Respond(sip.NewResponseFromRequest(request, sip.StatusOK, "OK", nil)); err != nil {
			t.Error(err)
		}
	})
	response := apiRequest(t, s.handler(), "POST", "/api/devices/"+queryTestDeviceID+"/query", `{"command":"DeviceInfo"}`, http.StatusOK)
	var result deviceQueryResponse
	if err := json.Unmarshal(response.Body.Bytes(), &result); err != nil {
		t.Fatal(err)
	}
	if result.Info == nil || result.Info.DeviceName != "first" {
		t.Fatalf("mismatch or duplicate replaced result: %s", response.Body.String())
	}
}

func TestDeviceQueryDeadlineAndRejection(t *testing.T) {
	for _, test := range []struct {
		name                  string
		sipStatus             int
		earlyResponse, cancel bool
		expected              int
	}{
		{name: "SIP 200 is not XML result", sipStatus: sip.StatusOK, expected: http.StatusGatewayTimeout},
		{name: "SIP rejected", sipStatus: sip.StatusForbidden, expected: http.StatusBadGateway},
		{name: "SIP rejected after XML", sipStatus: sip.StatusForbidden, earlyResponse: true, expected: http.StatusBadGateway},
		{name: "cancel with no result", cancel: true, expected: http.StatusBadGateway},
		{name: "cancel with early XML", cancel: true, earlyResponse: true, expected: http.StatusBadGateway},
	} {
		t.Run(test.name, func(t *testing.T) {
			s := testInfrastructure(t)
			ctx, cancel := context.WithTimeout(t.Context(), 350*time.Millisecond)
			defer cancel()
			newDeviceQueryPeer(t, s, func(query manscdpQuery, request *sip.Request, tx sip.ServerTransaction, peer *deviceQueryPeer) {
				if test.earlyResponse {
					peer.send(t, queryResponseXML(query, "OK", ""), query.DeviceID, sip.StatusOK)
				}
				if test.cancel {
					cancel()
				}
				if test.sipStatus != 0 {
					if err := tx.Respond(sip.NewResponseFromRequest(request, test.sipStatus, "Test result", nil)); err != nil {
						t.Error(err)
					}
				}
			})
			request := httptest.NewRequest("POST", "/api/devices/"+queryTestDeviceID+"/query", strings.NewReader(`{"command":"DeviceInfo"}`)).WithContext(ctx)
			request.Header.Set("Content-Type", "application/json")
			response := httptest.NewRecorder()
			s.handler().ServeHTTP(response, request)
			if response.Code != test.expected {
				t.Fatalf("HTTP %d want %d: %s", response.Code, test.expected, response.Body.String())
			}
			s.live.mu.Lock()
			pending := len(s.live.sip.pendingQueries)
			s.live.mu.Unlock()
			if pending != 0 {
				t.Fatal("canceled or rejected query retained")
			}
		})
	}
}

func TestDeviceQueryConcurrent(t *testing.T) {
	s := testInfrastructure(t)
	queries := make(chan manscdpQuery, 2)
	peer := newDeviceQueryPeer(t, s, func(query manscdpQuery, request *sip.Request, tx sip.ServerTransaction, _ *deviceQueryPeer) {
		queries <- query
		if err := tx.Respond(sip.NewResponseFromRequest(request, sip.StatusOK, "OK", nil)); err != nil {
			t.Error(err)
		}
	})
	var wait sync.WaitGroup
	for i := 0; i < 2; i++ {
		wait.Add(1)
		go func() {
			defer wait.Done()
			apiRequest(t, s.handler(), "POST", "/api/devices/"+queryTestDeviceID+"/query", `{"command":"DeviceInfo"}`, http.StatusOK)
		}()
	}
	seen := make(map[int]bool)
	for i := 0; i < 2; i++ {
		select {
		case query := <-queries:
			if seen[query.SN] {
				t.Error("concurrent queries reused SN")
			}
			seen[query.SN] = true
			peer.send(t, queryResponseXML(query, "OK", ""), query.DeviceID, sip.StatusOK)
		case <-time.After(time.Second):
			t.Error("concurrent query did not send")
		}
	}
	wait.Wait()
	s.live.mu.Lock()
	defer s.live.mu.Unlock()
	if len(s.live.sip.pendingQueries) != 0 {
		t.Fatal("concurrent queries retained")
	}
}

func TestDeviceQueryTotalDeadline(t *testing.T) {
	s := testInfrastructure(t)
	newDeviceQueryPeer(t, s, func(_ manscdpQuery, request *sip.Request, tx sip.ServerTransaction, _ *deviceQueryPeer) {
		if err := tx.Respond(sip.NewResponseFromRequest(request, sip.StatusOK, "OK", nil)); err != nil {
			t.Error(err)
		}
	})
	// 外层期限仅防止测试失控；5 秒必须由生产查询总期限触发，而非测试替它取消。
	ctx, cancel := context.WithTimeout(t.Context(), 8*time.Second)
	defer cancel()
	request := httptest.NewRequest("POST", "/api/devices/"+queryTestDeviceID+"/query", strings.NewReader(`{"command":"DeviceInfo"}`)).WithContext(ctx)
	request.Header.Set("Content-Type", "application/json")
	response := httptest.NewRecorder()
	start := time.Now()
	s.handler().ServeHTTP(response, request)
	if response.Code != http.StatusGatewayTimeout || ctx.Err() != nil || time.Since(start) < 4*time.Second || time.Since(start) > 7*time.Second {
		t.Fatalf("total deadline: HTTP=%d elapsed=%s outer=%v", response.Code, time.Since(start), ctx.Err())
	}
	s.live.mu.Lock()
	defer s.live.mu.Unlock()
	if len(s.live.sip.pendingQueries) != 0 {
		t.Fatal("timed out query retained")
	}
}

func TestDeviceStatusRejectsInvalidSuccess(t *testing.T) {
	s := testInfrastructure(t)
	newDeviceQueryPeer(t, s, func(query manscdpQuery, request *sip.Request, tx sip.ServerTransaction, peer *deviceQueryPeer) {
		for _, details := range []string{"", "<Online>OK</Online><Status>OK</Status>", "<Online>ONLINE</Online><Status>BAD</Status>"} {
			peer.send(t, queryResponseXML(query, "OK", details), query.DeviceID, sip.StatusBadRequest)
		}
		peer.send(t, queryResponseXML(query, "OK", "<Online>ONLINE</Online><Status>OK</Status>"), query.DeviceID, sip.StatusOK)
		if err := tx.Respond(sip.NewResponseFromRequest(request, sip.StatusOK, "OK", nil)); err != nil {
			t.Error(err)
		}
	})
	apiRequest(t, s.handler(), "POST", "/api/devices/"+queryTestDeviceID+"/query", `{"command":"DeviceStatus"}`, http.StatusOK)
}
