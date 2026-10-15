package main

import (
	"context"
	"encoding/json"
	"encoding/xml"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/emiago/sipgo"
	"github.com/emiago/sipgo/sip"
	"golang.org/x/text/encoding/simplifiedchinese"
)

const recordQueryBody = `{"start_time":"2026-10-15T00:00:00","end_time":"2026-10-15T01:00:00","type":"all"}`

const recordQueryPath = "/api/devices/" + queryTestDeviceID + "/channels/" + presetQueryChannelID + "/records/query"

func recordResponseXML(query manscdpQuery, details string) []byte {
	return []byte(fmt.Sprintf("<Response><CmdType>RecordInfo</CmdType><SN>%d</SN><DeviceID>%s</DeviceID><Name>camera</Name>%s</Response>", query.SN, query.DeviceID, details))
}

func recordItemXML(channelID, name string) string {
	return "<Item><DeviceID>" + channelID + "</DeviceID><Name>" + name + "</Name></Item>"
}

func assertNoDeviceQueries(t *testing.T, s *infrastructureServer) {
	t.Helper()
	s.live.mu.Lock()
	defer s.live.mu.Unlock()
	if len(s.live.sip.pendingQueries) != 0 {
		t.Fatalf("pending queries retained: %d", len(s.live.sip.pendingQueries))
	}
}

func waitRecordPeer(t *testing.T, done <-chan struct{}) {
	t.Helper()
	select {
	case <-done:
	case <-time.After(3 * time.Second):
		t.Fatal("record query peer did not finish")
	}
}

func TestRecordQueryRoute(t *testing.T) {
	s := testInfrastructure(t)
	if err := s.live.sip.deviceStore.create(t.Context(), gbDevice{deviceID: queryTestDeviceID, name: "record query"}); err != nil {
		t.Fatal(err)
	}
	apiRequest(t, s.handler(), "POST", "/api/devices/"+queryTestDeviceID+"/channels/"+presetQueryChannelID+"/records/query", recordQueryBody, http.StatusConflict)
}

func TestRecordQueryRequestValidation(t *testing.T) {
	s := testInfrastructure(t)
	for _, body := range []string{
		`{}`, `null`, recordQueryBody + `{}`, strings.Replace(recordQueryBody, `"all"`, `"ALL"`, 1),
		strings.Replace(recordQueryBody, `"all"`, `null`, 1),
		strings.Replace(recordQueryBody, `2026-10-15T00:00:00`, `2026-02-30T00:00:00`, 1),
		strings.Replace(recordQueryBody, `2026-10-15T00:00:00`, `2026-10-15T0:00:00`, 1),
		strings.Replace(recordQueryBody, `2026-10-15T00:00:00`, `2026-10-15T00:00:00Z`, 1),
		strings.Replace(recordQueryBody, `2026-10-15T00:00:00`, `2026-10-15T00:00:00.1`, 1),
		strings.Replace(recordQueryBody, `2026-10-15T01:00:00`, `2026-10-15T00:00:00`, 1),
		strings.Replace(recordQueryBody, `2026-10-15T01:00:00`, `2026-10-14T23:00:00`, 1),
		strings.Replace(recordQueryBody, `"type"`, `"command"`, 1),
	} {
		apiRequest(t, s.handler(), "POST", recordQueryPath, body, http.StatusBadRequest)
	}
	for _, path := range []string{strings.Replace(recordQueryPath, queryTestDeviceID, "invalid", 1), strings.Replace(recordQueryPath, presetQueryChannelID, "invalid", 1)} {
		apiRequest(t, s.handler(), "POST", path, recordQueryBody, http.StatusBadRequest)
	}
	apiRequest(t, s.handler(), "POST", recordQueryPath, recordQueryBody, http.StatusNotFound)
	assertNoDeviceQueries(t, s)
	if s.live.sip.messageSN.Load() != 0 {
		t.Fatal("invalid request allocated query")
	}
}

func TestRecordQueryHTTPResults(t *testing.T) {
	for _, test := range []struct {
		name    string
		details []string
		want    int
		gbk     bool
	}{
		{name: "zero without RecordList", details: []string{`<SumNum>0</SumNum>`}},
		{name: "zero with empty list", details: []string{`<SumNum>0</SumNum><RecordList Num="0"/>`}},
		{name: "positive total empty progress batch", details: []string{`<SumNum>1</SumNum><RecordList Num="0"/>`, `<SumNum>1</SumNum><RecordList Num="1">` + recordItemXML(presetQueryChannelID, "later") + `</RecordList>`}, want: 1},
		{name: "optional fields omitted", details: []string{`<SumNum>1</SumNum><RecordList Num="1">` + recordItemXML(presetQueryChannelID, "") + `</RecordList>`}, want: 1},
		{name: "GBK and escaped fields", details: []string{`<SumNum>1</SumNum><RecordList Num="1"><Item><DeviceID>` + presetQueryChannelID + `</DeviceID><Name>北门&amp;录像</Name><FilePath>/a&amp;b</FilePath><Address>地址</Address><StartTime>2026-10-15T00:00:01</StartTime><EndTime>2026-10-15T00:00:02</EndTime><Secrecy>01</Secrecy><Type>alarm</Type><RecorderID>recorder</RecorderID><FileSize>001234</FileSize></Item></RecordList>`}, want: 1, gbk: true},
		{name: "multiple early batches", details: []string{`<SumNum>3</SumNum><RecordList Num="1">` + recordItemXML(presetQueryChannelID, "first") + `</RecordList>`, `<SumNum>3</SumNum><RecordList Num="2">` + recordItemXML(presetQueryChannelID, "second") + recordItemXML(presetQueryChannelID, "third") + `</RecordList>`}, want: 3},
		// 新事务的相同条目可能是合法重复，也可能是设备错误重发，协议无法区分。
		{name: "new transactions preserve identical items", details: []string{`<SumNum>2</SumNum><RecordList Num="1">` + recordItemXML(presetQueryChannelID, "same") + `</RecordList>`, `<SumNum>2</SumNum><RecordList Num="1">` + recordItemXML(presetQueryChannelID, "same") + `</RecordList>`}, want: 2},
	} {
		t.Run(test.name, func(t *testing.T) {
			s := testInfrastructure(t)
			done := make(chan struct{})
			peer := newDeviceQueryPeer(t, s, func(query manscdpQuery, request *sip.Request, tx sip.ServerTransaction, peer *deviceQueryPeer) {
				defer close(done)
				if query.CmdType != "RecordInfo" || query.DeviceID != presetQueryChannelID || query.StartTime != "2026-10-15T00:00:00" || query.EndTime != "2026-10-15T01:00:00" || query.Type != "all" || request.To().Address.User != queryTestDeviceID {
					t.Errorf("query target/parameters: %+v", query)
				}
				for _, details := range test.details {
					body := recordResponseXML(query, details)
					if test.gbk {
						var err error
						body, err = simplifiedchinese.GBK.NewEncoder().Bytes(append([]byte(`<?xml version="1.0" encoding="GB2312"?>`), body...))
						if err != nil {
							t.Error(err)
							return
						}
					}
					peer.send(t, body, queryTestDeviceID, sip.StatusOK)
				}
				if err := tx.Respond(sip.NewResponseFromRequest(request, sip.StatusOK, "OK", nil)); err != nil {
					t.Error(err)
				}
			})
			t.Cleanup(func() { waitRecordPeer(t, done) })
			addPresetQueryChannels(t, s, presetQueryChannelID)
			response := apiRequest(t, s.handler(), "POST", recordQueryPath, recordQueryBody, http.StatusOK)
			waitRecordPeer(t, done)
			var result deviceQueryResponse
			if err := json.Unmarshal(response.Body.Bytes(), &result); err != nil {
				t.Fatal(err)
			}
			if result.DeviceID != queryTestDeviceID || result.ChannelID != presetQueryChannelID || result.Command != "RecordInfo" || result.SN <= 0 || result.SumNum == nil || *result.SumNum != test.want || result.Records == nil || len(*result.Records) != test.want || result.Info != nil || result.Status != nil || result.Presets != nil || strings.Contains(response.Body.String(), `"result"`) || strings.Contains(response.Body.String(), `"records":null`) {
				t.Fatalf("result: %s", response.Body.String())
			}
			if test.name == "multiple early batches" {
				for index, name := range []string{"first", "second", "third"} {
					if *(*result.Records)[index].Name != name {
						t.Fatalf("receive order: %s", response.Body.String())
					}
				}
			}
			if test.name == "optional fields omitted" && ((*result.Records)[0].Secrecy != "0" || (*result.Records)[0].StartTime != "" || *(*result.Records)[0].Name != "") {
				t.Fatalf("optional fields: %s", response.Body.String())
			}
			if test.gbk && (*(*result.Records)[0].Name != "北门&录像" || (*result.Records)[0].FileSize != "001234" || (*result.Records)[0].Secrecy != "01" || (*result.Records)[0].FilePath != "/a&b") {
				t.Fatalf("raw fields/GBK: %s", response.Body.String())
			}
			assertNoDeviceQueries(t, s)
			peer.send(t, recordResponseXML(manscdpQuery{SN: result.SN, DeviceID: presetQueryChannelID}, `<SumNum>0</SumNum>`), queryTestDeviceID, sip.StatusOK)
		})
	}
}

func TestRecordQueryBadBatchDoesNotPollute(t *testing.T) {
	s := testInfrastructure(t)
	done := make(chan struct{})
	newDeviceQueryPeer(t, s, func(query manscdpQuery, request *sip.Request, tx sip.ServerTransaction, peer *deviceQueryPeer) {
		defer close(done)
		for _, details := range []string{
			``, `<SumNum>-1</SumNum>`, `<SumNum>1</SumNum>`, `<SumNum>0</SumNum><Result>OK</Result>`,
			`<SumNum>1</SumNum><RecordList/>`, `<SumNum>1</SumNum><RecordList Num="-1"/>`,
			`<SumNum>1</SumNum><RecordList Num="1"/>`,
			`<SumNum>1</SumNum><RecordList Num="1"><Item><DeviceID>` + presetQueryChannelID + `</DeviceID></Item></RecordList>`,
			`<SumNum>1</SumNum><RecordList Num="1">` + recordItemXML(queryTestDeviceID, "wrong target") + `</RecordList>`,
		} {
			peer.send(t, recordResponseXML(query, details), queryTestDeviceID, sip.StatusBadRequest)
		}
		peer.send(t, []byte(strings.Replace(string(recordResponseXML(query, `<SumNum>0</SumNum>`)), "<Name>camera</Name>", "", 1)), queryTestDeviceID, sip.StatusBadRequest)
		peer.send(t, recordResponseXML(query, `<SumNum>2</SumNum><RecordList Num="1">`+recordItemXML(presetQueryChannelID, "first")+`</RecordList>`), queryTestDeviceID, sip.StatusOK)
		for _, details := range []string{
			`<SumNum>3</SumNum><RecordList Num="1">` + recordItemXML(presetQueryChannelID, "changed total") + `</RecordList>`,
			`<SumNum>1001</SumNum><RecordList Num="1">` + recordItemXML(presetQueryChannelID, "changed total above limit") + `</RecordList>`,
			`<SumNum>2</SumNum><RecordList Num="2">` + recordItemXML(presetQueryChannelID, "overflow") + recordItemXML(presetQueryChannelID, "overflow") + `</RecordList>`,
		} {
			peer.send(t, recordResponseXML(query, details), queryTestDeviceID, sip.StatusBadRequest)
		}
		peer.send(t, recordResponseXML(query, `<SumNum>2</SumNum><RecordList Num="1">`+recordItemXML(presetQueryChannelID, "last")+`</RecordList>`), queryTestDeviceID, sip.StatusOK)
		if err := tx.Respond(sip.NewResponseFromRequest(request, sip.StatusOK, "OK", nil)); err != nil {
			t.Error(err)
		}
	})
	t.Cleanup(func() { waitRecordPeer(t, done) })
	addPresetQueryChannels(t, s, presetQueryChannelID)
	response := apiRequest(t, s.handler(), "POST", recordQueryPath, recordQueryBody, http.StatusOK)
	waitRecordPeer(t, done)
	var result deviceQueryResponse
	if err := json.Unmarshal(response.Body.Bytes(), &result); err != nil {
		t.Fatal(err)
	}
	if result.Records == nil || len(*result.Records) != 2 || *(*result.Records)[0].Name != "first" || *(*result.Records)[1].Name != "last" {
		t.Fatalf("bad batch polluted results: %s", response.Body.String())
	}
	assertNoDeviceQueries(t, s)
}

func TestRecordQueryDeclaredLimit(t *testing.T) {
	s := testInfrastructure(t)
	done := make(chan struct{})
	newDeviceQueryPeer(t, s, func(query manscdpQuery, request *sip.Request, tx sip.ServerTransaction, peer *deviceQueryPeer) {
		defer close(done)
		peer.send(t, recordResponseXML(query, `<SumNum>1001</SumNum><RecordList Num="1">`+recordItemXML(presetQueryChannelID, "first")+`</RecordList>`), queryTestDeviceID, sip.StatusOK)
		if err := tx.Respond(sip.NewResponseFromRequest(request, sip.StatusOK, "OK", nil)); err != nil {
			t.Error(err)
		}
	})
	t.Cleanup(func() { waitRecordPeer(t, done) })
	addPresetQueryChannels(t, s, presetQueryChannelID)
	response := apiRequest(t, s.handler(), "POST", recordQueryPath, recordQueryBody, http.StatusUnprocessableEntity)
	waitRecordPeer(t, done)
	if response.Body.String() != "{\"error\":\"record_result_too_large\"}\n" {
		t.Fatal(response.Body.String())
	}
	assertNoDeviceQueries(t, s)
}

func TestRecordQueryOldWireFormatsUnchanged(t *testing.T) {
	for _, command := range []string{"Catalog", "DeviceInfo", "DeviceStatus", "PresetQuery"} {
		body, err := xml.Marshal(manscdpQuery{CmdType: command, SN: 42, DeviceID: queryTestDeviceID})
		if err != nil || string(body) != "<Query><CmdType>"+command+"</CmdType><SN>42</SN><DeviceID>"+queryTestDeviceID+"</DeviceID></Query>" {
			t.Fatalf("%s XML changed: %s %v", command, body, err)
		}
	}
	for _, test := range []struct {
		response deviceQueryResponse
		want     string
	}{
		{deviceQueryResponse{DeviceID: "parent", Command: "DeviceInfo", SN: 1, Result: "OK", Info: &deviceInfo{}}, `{"device_id":"parent","command":"DeviceInfo","sn":1,"result":"OK","info":{"device_name":"","manufacturer":"","model":"","firmware":""}}`},
		{deviceQueryResponse{DeviceID: "parent", Command: "DeviceStatus", SN: 2, Result: "ERROR"}, `{"device_id":"parent","command":"DeviceStatus","sn":2,"result":"ERROR"}`},
		{deviceQueryResponse{DeviceID: "parent", ChannelID: "child", Command: "PresetQuery", SN: 3, Presets: &[]devicePreset{}}, `{"device_id":"parent","channel_id":"child","command":"PresetQuery","sn":3,"presets":[]}`},
	} {
		body, err := json.Marshal(test.response)
		if err != nil || string(body) != test.want {
			t.Fatalf("old JSON changed: %s want %s: %v", body, test.want, err)
		}
	}
}

func TestRecordQuerySameTransactionRetransmission(t *testing.T) {
	s := testInfrastructure(t)
	var calls atomic.Int32
	s.live.sip.server.OnMessage(func(request *sip.Request, tx sip.ServerTransaction) {
		calls.Add(1)
		s.live.sip.handleMessage(request, tx)
	})
	done := make(chan struct{})
	newDeviceQueryPeer(t, s, func(query manscdpQuery, original *sip.Request, tx sip.ServerTransaction, peer *deviceQueryPeer) {
		defer close(done)
		request := sip.NewRequest(sip.MESSAGE, sip.Uri{Scheme: "sip", User: peer.platformID, Host: "127.0.0.1"})
		request.AppendHeader(&sip.FromHeader{Address: sip.Uri{Scheme: "sip", User: queryTestDeviceID, Host: "127.0.0.1"}, Params: sip.NewParams()})
		request.AppendHeader(&sip.ToHeader{Address: request.Recipient})
		request.AppendHeader(sip.NewHeader("Content-Type", "Application/MANSCDP+xml"))
		request.SetBody(recordResponseXML(query, `<SumNum>2</SumNum><RecordList Num="1">`+recordItemXML(presetQueryChannelID, "first")+`</RecordList>`))
		request.SetTransport("UDP")
		request.SetDestination(peer.platform)
		ctx, cancel := context.WithTimeout(t.Context(), 2*time.Second)
		defer cancel()
		response, err := peer.client.Do(ctx, request)
		if err != nil || response.StatusCode != sip.StatusOK {
			t.Errorf("first datagram: %v %v", response, err)
			return
		}
		wire := request.String()
		response, err = peer.client.Do(ctx, request)
		if err != nil || response.StatusCode != sip.StatusOK || request.String() != wire {
			t.Errorf("same datagram replay: %v %v changed=%v", response, err, request.String() != wire)
			return
		}
		if calls.Load() != 1 {
			t.Errorf("wire-identical retransmission entered handler %d times", calls.Load())
		}
		s.live.mu.Lock()
		pending := s.live.sip.pendingQueries[query.SN]
		if len(pending.records) != 1 || len(pending.response) != 0 {
			t.Errorf("retransmission counted twice: %+v", pending)
		}
		s.live.mu.Unlock()
		peer.send(t, recordResponseXML(query, `<SumNum>2</SumNum><RecordList Num="1">`+recordItemXML(presetQueryChannelID, "second")+`</RecordList>`), queryTestDeviceID, sip.StatusOK)
		if err := tx.Respond(sip.NewResponseFromRequest(original, sip.StatusOK, "OK", nil)); err != nil {
			t.Error(err)
		}
	})
	t.Cleanup(func() { waitRecordPeer(t, done) })
	addPresetQueryChannels(t, s, presetQueryChannelID)
	response := apiRequest(t, s.handler(), "POST", recordQueryPath, recordQueryBody, http.StatusOK)
	waitRecordPeer(t, done)
	if calls.Load() != 2 || !strings.Contains(response.Body.String(), `"name":"second"`) {
		t.Fatalf("final retransmission result: calls=%d %s", calls.Load(), response.Body.String())
	}
	assertNoDeviceQueries(t, s)
}

func TestRecordQueryCorrelationAndCurrentSource(t *testing.T) {
	s := testInfrastructure(t)
	done := make(chan struct{})
	newDeviceQueryPeer(t, s, func(query manscdpQuery, request *sip.Request, tx sip.ServerTransaction, peer *deviceQueryPeer) {
		defer close(done)
		wrong := query
		wrong.SN++
		peer.send(t, recordResponseXML(wrong, `<SumNum>0</SumNum>`), queryTestDeviceID, sip.StatusOK)
		wrong = query
		wrong.DeviceID = "34020000001320000003"
		peer.send(t, recordResponseXML(wrong, `<SumNum>0</SumNum>`), queryTestDeviceID, sip.StatusOK)
		wrong = query
		wrong.CmdType, wrong.DeviceID = "DeviceInfo", queryTestDeviceID
		peer.send(t, queryResponseXML(wrong, "OK", ""), queryTestDeviceID, sip.StatusOK)
		device, _ := s.live.sip.devices.getOnline(queryTestDeviceID, time.Now())
		other := device
		other.id = "34020000001320000004"
		s.live.sip.devices.register(other)
		peer.send(t, recordResponseXML(query, `<SumNum>0</SumNum>`), other.id, sip.StatusForbidden)
		peer.send(t, recordResponseXML(query, `<SumNum>0</SumNum>`), presetQueryChannelID, sip.StatusForbidden)
		ua, err := sipgo.NewUA()
		if err != nil {
			t.Error(err)
			return
		}
		client, err := sipgo.NewClient(ua)
		if err != nil {
			_ = ua.Close()
			t.Error(err)
			return
		}
		outsider := *peer
		outsider.client = client
		outsider.send(t, recordResponseXML(query, `<SumNum>0</SumNum>`), queryTestDeviceID, sip.StatusForbidden)
		_ = ua.Close()
		// 当前注册端点已变更，旧端点不能完成仍在等待的查询。
		changed := device
		changed.remoteEndpoint = "127.0.0.1:1"
		s.live.sip.devices.register(changed)
		peer.send(t, recordResponseXML(query, `<SumNum>0</SumNum>`), queryTestDeviceID, sip.StatusForbidden)
		s.live.sip.devices.register(device)
		s.live.mu.Lock()
		pending := s.live.sip.pendingQueries[query.SN]
		if pending.records != nil || len(pending.response) != 0 {
			t.Errorf("wrong correlation/source polluted query: %+v", pending)
		}
		s.live.mu.Unlock()
		peer.send(t, recordResponseXML(query, `<SumNum>0</SumNum>`), queryTestDeviceID, sip.StatusOK)
		if err := tx.Respond(sip.NewResponseFromRequest(request, sip.StatusOK, "OK", nil)); err != nil {
			t.Error(err)
		}
	})
	t.Cleanup(func() { waitRecordPeer(t, done) })
	addPresetQueryChannels(t, s, presetQueryChannelID)
	apiRequest(t, s.handler(), "POST", recordQueryPath, recordQueryBody, http.StatusOK)
	waitRecordPeer(t, done)
	assertNoDeviceQueries(t, s)
}

func TestRecordQueryDeadlineCancelAndRejection(t *testing.T) {
	for _, test := range []struct {
		name                   string
		status                 int
		early, partial, cancel bool
		timeout                time.Duration
		want                   int
	}{
		{name: "SIP 200 alone times out after actual fifteen seconds", status: sip.StatusOK, want: http.StatusGatewayTimeout},
		{name: "incomplete response times out", status: sip.StatusOK, partial: true, timeout: 200 * time.Millisecond, want: http.StatusGatewayTimeout},
		{name: "reject", status: sip.StatusForbidden, want: http.StatusBadGateway},
		{name: "reject after early XML", status: sip.StatusForbidden, early: true, want: http.StatusBadGateway},
		{name: "cancel", status: sip.StatusOK, cancel: true, want: http.StatusBadGateway},
	} {
		t.Run(test.name, func(t *testing.T) {
			s := testInfrastructure(t)
			ctx, cancel := context.WithCancel(t.Context())
			defer cancel()
			if test.timeout > 0 {
				var deadlineCancel context.CancelFunc
				ctx, deadlineCancel = context.WithTimeout(ctx, test.timeout)
				defer deadlineCancel()
			}
			done := make(chan struct{})
			peer := newDeviceQueryPeer(t, s, func(query manscdpQuery, request *sip.Request, tx sip.ServerTransaction, peer *deviceQueryPeer) {
				defer close(done)
				if test.early {
					peer.send(t, recordResponseXML(query, `<SumNum>0</SumNum>`), queryTestDeviceID, sip.StatusOK)
				}
				if test.partial {
					peer.send(t, recordResponseXML(query, `<SumNum>2</SumNum><RecordList Num="1">`+recordItemXML(presetQueryChannelID, "first")+`</RecordList>`), queryTestDeviceID, sip.StatusOK)
				}
				if test.cancel {
					cancel()
				}
				if err := tx.Respond(sip.NewResponseFromRequest(request, test.status, "Test", nil)); err != nil {
					t.Error(err)
				}
			})
			t.Cleanup(func() { waitRecordPeer(t, done) })
			addPresetQueryChannels(t, s, presetQueryChannelID)
			request := httptest.NewRequest("POST", recordQueryPath, strings.NewReader(recordQueryBody)).WithContext(ctx)
			request.Header.Set("Content-Type", "application/json")
			response := httptest.NewRecorder()
			start := time.Now()
			s.handler().ServeHTTP(response, request)
			elapsed := time.Since(start)
			waitRecordPeer(t, done)
			if response.Code != test.want {
				t.Fatalf("HTTP %d want %d: %s", response.Code, test.want, response.Body.String())
			}
			if test.name == "SIP 200 alone times out after actual fifteen seconds" && (elapsed < 14*time.Second || elapsed > 18*time.Second) {
				t.Fatalf("RecordInfo total deadline is not fifteen seconds: %s", elapsed)
			}
			assertNoDeviceQueries(t, s)
			peer.send(t, recordResponseXML(manscdpQuery{SN: 1, DeviceID: presetQueryChannelID}, `<SumNum>0</SumNum>`), queryTestDeviceID, sip.StatusOK)
		})
	}
}

func TestRecordQueryConcurrentChannelsAndOldQueries(t *testing.T) {
	s := testInfrastructure(t)
	queries := make(chan manscdpQuery, 3)
	var callbacks sync.WaitGroup
	callbacks.Add(3)
	peer := newDeviceQueryPeer(t, s, func(query manscdpQuery, request *sip.Request, tx sip.ServerTransaction, _ *deviceQueryPeer) {
		defer callbacks.Done()
		queries <- query
		if err := tx.Respond(sip.NewResponseFromRequest(request, sip.StatusOK, "OK", nil)); err != nil {
			t.Error(err)
		}
	})
	addPresetQueryChannels(t, s, presetQueryChannelID, "34020000001320000003")
	var requests sync.WaitGroup
	requests.Add(3)
	for _, channelID := range []string{presetQueryChannelID, "34020000001320000003", ""} {
		go func() {
			defer requests.Done()
			path, body := recordQueryPath, recordQueryBody
			if channelID == "" {
				path, body = "/api/devices/"+queryTestDeviceID+"/query", `{"command":"DeviceInfo"}`
			} else {
				path = strings.Replace(path, presetQueryChannelID, channelID, 1)
			}
			ctx, cancel := context.WithTimeout(t.Context(), 3*time.Second)
			defer cancel()
			request := httptest.NewRequest("POST", path, strings.NewReader(body)).WithContext(ctx)
			request.Header.Set("Content-Type", "application/json")
			response := httptest.NewRecorder()
			s.handler().ServeHTTP(response, request)
			if response.Code != http.StatusOK {
				t.Errorf("concurrent %s: HTTP %d %s", path, response.Code, response.Body.String())
				return
			}
			var result deviceQueryResponse
			if err := json.Unmarshal(response.Body.Bytes(), &result); err != nil || result.ChannelID != channelID || (channelID != "" && (result.Records == nil || len(*result.Records) != 2 || (*result.Records)[0].DeviceID != channelID)) || (channelID == "" && (result.Info == nil || result.Records != nil)) {
				t.Errorf("crossed result: %s %v", response.Body.String(), err)
			}
		}()
	}
	var batch []manscdpQuery
	for range 3 {
		select {
		case query := <-queries:
			batch = append(batch, query)
		case <-time.After(3 * time.Second):
			t.Fatal("concurrent query not received")
		}
	}
	callbacks.Wait()
	for _, query := range batch {
		if query.CmdType == "RecordInfo" {
			peer.send(t, recordResponseXML(query, `<SumNum>2</SumNum><RecordList Num="1">`+recordItemXML(query.DeviceID, "first")+`</RecordList>`), queryTestDeviceID, sip.StatusOK)
		}
	}
	for index := len(batch) - 1; index >= 0; index-- {
		query := batch[index]
		if query.CmdType == "RecordInfo" {
			peer.send(t, recordResponseXML(query, `<SumNum>2</SumNum><RecordList Num="1">`+recordItemXML(query.DeviceID, "second")+`</RecordList>`), queryTestDeviceID, sip.StatusOK)
		} else {
			peer.send(t, queryResponseXML(query, "OK", ""), queryTestDeviceID, sip.StatusOK)
		}
	}
	requests.Wait()
	assertNoDeviceQueries(t, s)
}

func TestRecordQueryThousandItems(t *testing.T) {
	s := testInfrastructure(t)
	done := make(chan struct{})
	newDeviceQueryPeer(t, s, func(query manscdpQuery, request *sip.Request, tx sip.ServerTransaction, peer *deviceQueryPeer) {
		defer close(done)
		// 小批次遵守当前 sipgo UDP MTU，不调整全局传输参数。
		for range 200 {
			items := strings.Repeat(recordItemXML(presetQueryChannelID, "same"), 5)
			peer.send(t, recordResponseXML(query, `<SumNum>1000</SumNum><RecordList Num="5">`+items+`</RecordList>`), queryTestDeviceID, sip.StatusOK)
		}
		if err := tx.Respond(sip.NewResponseFromRequest(request, sip.StatusOK, "OK", nil)); err != nil {
			t.Error(err)
		}
	})
	t.Cleanup(func() { waitRecordPeer(t, done) })
	addPresetQueryChannels(t, s, presetQueryChannelID)
	response := apiRequest(t, s.handler(), "POST", recordQueryPath, recordQueryBody, http.StatusOK)
	waitRecordPeer(t, done)
	var result deviceQueryResponse
	if err := json.Unmarshal(response.Body.Bytes(), &result); err != nil || result.SumNum == nil || *result.SumNum != 1000 || result.Records == nil || len(*result.Records) != 1000 {
		t.Fatalf("1000 item bound: %s %v", response.Body.String(), err)
	}
	assertNoDeviceQueries(t, s)
}

func TestRecordQueryHTTPPreconditions(t *testing.T) {
	for _, test := range []struct {
		name      string
		configure func(*infrastructureServer)
		want      int
		code      string
	}{
		{name: "missing channel", want: http.StatusNotFound, code: "channel_not_found"},
		{name: "stopping before offline and channel", configure: func(s *infrastructureServer) {
			s.live.mu.Lock()
			s.live.stoppingDevices[queryTestDeviceID] = struct{}{}
			s.live.mu.Unlock()
			s.live.sip.devices.unregister(queryTestDeviceID)
		}, want: http.StatusConflict, code: "device_stopping"},
		{name: "offline after catalog removal", configure: func(s *infrastructureServer) {
			addPresetQueryChannels(t, s, presetQueryChannelID)
			s.live.sip.devices.unregister(queryTestDeviceID)
			s.live.sip.channels.removeDevice(queryTestDeviceID)
		}, want: http.StatusConflict, code: "device_offline"},
	} {
		t.Run(test.name, func(t *testing.T) {
			s := testInfrastructure(t)
			newDeviceQueryPeer(t, s, func(manscdpQuery, *sip.Request, sip.ServerTransaction, *deviceQueryPeer) {
				t.Error("precondition issued SIP")
			})
			if test.configure != nil {
				test.configure(s)
			}
			response := apiRequest(t, s.handler(), "POST", recordQueryPath, recordQueryBody, test.want)
			if !strings.Contains(response.Body.String(), test.code) || s.live.sip.messageSN.Load() != 0 {
				t.Fatalf("precondition: %s sn=%d", response.Body.String(), s.live.sip.messageSN.Load())
			}
			assertNoDeviceQueries(t, s)
		})
	}
}
