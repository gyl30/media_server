package main

import (
	"bytes"
	"encoding/xml"
	"fmt"
	"io"
	"testing"
	"time"

	"github.com/emiago/sipgo"
	"github.com/emiago/sipgo/sip"
	"golang.org/x/text/encoding/simplifiedchinese"
	"golang.org/x/text/transform"
)

const testChannelID = "34020000001320000002"

func TestRegistrationTriggersCatalogQuery(t *testing.T) {
	cfg := testConfig()
	_, addr := startRegistrar(t, cfg)
	client, queries := newCatalogDevice(t)
	registerDevice(t, client, cfg, addr, testDeviceID, 120)

	select {
	case request := <-queries:
		if request.Method != sip.MESSAGE || request.ContentType() == nil || request.ContentType().Value() != "Application/MANSCDP+xml" {
			t.Fatalf("query = %s content-type = %v", request.StartLine(), request.ContentType())
		}
		var query catalogQuery
		if err := xml.Unmarshal(request.Body(), &query); err != nil {
			t.Fatalf("xml.Unmarshal() error = %v", err)
		}
		if query.XMLName.Local != "Query" || query.CmdType != "Catalog" || query.DeviceID != testDeviceID || query.SN <= 0 {
			t.Fatalf("query = %+v", query)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("Catalog query was not received")
	}
}

func TestCatalogSingleAndStatusUpdate(t *testing.T) {
	cfg := testConfig()
	server, addr := startRegistrar(t, cfg)
	client := newRegisterClient(t)
	registerDevice(t, client, cfg, addr, testDeviceID, 120)

	server.channels.beginQuery(testDeviceID, 101)
	response := doSIP(t, client, newMessageRequest(t, cfg, addr, testDeviceID, catalogXML(101, 1, []catalogChannel{{
		DeviceID: testChannelID,
		Name:     "front gate",
		ParentID: testDeviceID,
		Status:   "ON",
	}})))
	if response.StatusCode != sip.StatusOK {
		t.Fatalf("Catalog status = %d", response.StatusCode)
	}
	channel, ok := server.channels.get(testDeviceID, testChannelID)
	if !ok || channel.name != "front gate" || channel.status != "ON" {
		t.Fatalf("channel = %+v, exists = %v", channel, ok)
	}

	server.channels.beginQuery(testDeviceID, 102)
	response = doSIP(t, client, newMessageRequest(t, cfg, addr, testDeviceID, catalogXML(102, 1, []catalogChannel{{
		DeviceID: testChannelID,
		Name:     "front gate",
		ParentID: testDeviceID,
		Status:   "OFF",
	}})))
	if response.StatusCode != sip.StatusOK {
		t.Fatalf("Catalog update status = %d", response.StatusCode)
	}
	channel, _ = server.channels.get(testDeviceID, testChannelID)
	if channel.status != "OFF" || server.channels.len(testDeviceID) != 1 {
		t.Fatalf("updated channel = %+v count = %d", channel, server.channels.len(testDeviceID))
	}
}

func TestCatalogMultiMessageReplacesAfterComplete(t *testing.T) {
	cfg := testConfig()
	server, addr := startRegistrar(t, cfg)
	client := newRegisterClient(t)
	registerDevice(t, client, cfg, addr, testDeviceID, 120)
	server.channels.beginQuery(testDeviceID, 201)

	first := catalogChannel{DeviceID: testChannelID, Name: "one", ParentID: testDeviceID, Status: "ON"}
	second := catalogChannel{DeviceID: "34020000001320000003", Name: "two", ParentID: testDeviceID, Status: "ON"}
	if response := doSIP(t, client, newMessageRequest(t, cfg, addr, testDeviceID, catalogXML(201, 2, []catalogChannel{first}))); response.StatusCode != sip.StatusOK {
		t.Fatalf("first chunk status = %d", response.StatusCode)
	}
	if server.channels.len(testDeviceID) != 0 {
		t.Fatal("partial Catalog became visible")
	}
	if response := doSIP(t, client, newMessageRequest(t, cfg, addr, testDeviceID, catalogXML(201, 2, []catalogChannel{second}))); response.StatusCode != sip.StatusOK {
		t.Fatalf("second chunk status = %d", response.StatusCode)
	}
	if server.channels.len(testDeviceID) != 2 {
		t.Fatalf("channel count = %d", server.channels.len(testDeviceID))
	}
}

func TestCatalogEmptyAndGB2312(t *testing.T) {
	cfg := testConfig()
	server, addr := startRegistrar(t, cfg)
	client := newRegisterClient(t)
	registerDevice(t, client, cfg, addr, testDeviceID, 120)

	server.channels.beginQuery(testDeviceID, 301)
	utf8Body := catalogXML(301, 1, []catalogChannel{{DeviceID: testChannelID, Name: "摄像机", ParentID: testDeviceID, Status: "ON"}})
	utf8Body = bytes.Replace(utf8Body, []byte(`encoding="UTF-8"`), []byte(`encoding="GB2312"`), 1)
	reader := transform.NewReader(bytes.NewReader(utf8Body), simplifiedchinese.GBK.NewEncoder())
	gb2312Body, err := io.ReadAll(reader)
	if err != nil {
		t.Fatalf("GB2312 encode error = %v", err)
	}
	if response := doSIP(t, client, newMessageRequest(t, cfg, addr, testDeviceID, gb2312Body)); response.StatusCode != sip.StatusOK {
		t.Fatalf("GB2312 Catalog status = %d", response.StatusCode)
	}
	channel, _ := server.channels.get(testDeviceID, testChannelID)
	if channel.name != "摄像机" {
		t.Fatalf("channel name = %q", channel.name)
	}

	server.channels.beginQuery(testDeviceID, 302)
	if response := doSIP(t, client, newMessageRequest(t, cfg, addr, testDeviceID, catalogXML(302, 0, nil))); response.StatusCode != sip.StatusOK {
		t.Fatalf("empty Catalog status = %d", response.StatusCode)
	}
	if server.channels.len(testDeviceID) != 0 {
		t.Fatalf("empty Catalog count = %d", server.channels.len(testDeviceID))
	}
}

func TestCatalogRejectsMalformedOrUnsolicitedResponse(t *testing.T) {
	cfg := testConfig()
	server, addr := startRegistrar(t, cfg)
	client := newRegisterClient(t)
	registerDevice(t, client, cfg, addr, testDeviceID, 120)

	server.channels.beginQuery(testDeviceID, 401)
	for name, body := range map[string][]byte{
		"wrong device":  catalogResponseXML("34020000001320000009", 401, 0, nil),
		"invalid item":  catalogXML(401, 1, []catalogChannel{{DeviceID: "bad", Status: "ON"}}),
		"invalid count": catalogResponseXML(testDeviceID, 401, 0, []catalogChannel{{DeviceID: testChannelID, Status: "ON"}}),
		"malformed XML": []byte("<Response><CmdType>Catalog"),
		"unsolicited":   catalogXML(999, 0, nil),
	} {
		t.Run(name, func(t *testing.T) {
			response := doSIP(t, client, newMessageRequest(t, cfg, addr, testDeviceID, body))
			if response.StatusCode == sip.StatusOK {
				t.Fatalf("status = %d", response.StatusCode)
			}
		})
	}
}

func TestCatalogPendingQueryExpires(t *testing.T) {
	registry := newChannelRegistry()
	now := time.Date(2026, 8, 29, 12, 0, 0, 0, time.UTC)
	registry.now = func() time.Time { return now }
	registry.pendingTimeout = 5 * time.Second
	registry.beginQuery(testDeviceID, 77)
	if registry.pendingLen() != 1 {
		t.Fatalf("pending count = %d", registry.pendingLen())
	}
	now = now.Add(5 * time.Second)
	if err := registry.apply(catalogResponse{
		CmdType: "Catalog", SN: 77, DeviceID: testDeviceID, SumNum: 0,
		DeviceList: struct {
			Num   int              `xml:"Num,attr"`
			Items []catalogChannel `xml:"Item"`
		}{},
	}); err == nil {
		t.Fatal("expired pending query accepted response")
	}
	if registry.pendingLen() != 0 {
		t.Fatalf("expired pending count = %d", registry.pendingLen())
	}
}

func TestCatalogInvalidChunkDoesNotPollutePendingQuery(t *testing.T) {
	registry := newChannelRegistry()
	registry.beginQuery(testDeviceID, 88)
	if err := registry.apply(catalogResponse{
		CmdType: "Catalog", SN: 88, DeviceID: testDeviceID, SumNum: 2,
		DeviceList: struct {
			Num   int              `xml:"Num,attr"`
			Items []catalogChannel `xml:"Item"`
		}{Num: 1, Items: []catalogChannel{{DeviceID: testChannelID, Status: "ON"}}},
	}); err != nil {
		t.Fatalf("first apply() error = %v", err)
	}
	if err := registry.apply(catalogResponse{
		CmdType: "Catalog", SN: 88, DeviceID: testDeviceID, SumNum: 2,
		DeviceList: struct {
			Num   int              `xml:"Num,attr"`
			Items []catalogChannel `xml:"Item"`
		}{Num: 2, Items: []catalogChannel{{DeviceID: "34020000001320000003", Status: "ON"}, {DeviceID: "invalid", Status: "ON"}}},
	}); err == nil {
		t.Fatal("invalid apply() succeeded")
	}
	if err := registry.apply(catalogResponse{
		CmdType: "Catalog", SN: 88, DeviceID: testDeviceID, SumNum: 2,
		DeviceList: struct {
			Num   int              `xml:"Num,attr"`
			Items []catalogChannel `xml:"Item"`
		}{Num: 1, Items: []catalogChannel{{DeviceID: "34020000001320000004", Status: "ON"}}},
	}); err != nil {
		t.Fatalf("final apply() error = %v", err)
	}
	if registry.len(testDeviceID) != 2 {
		t.Fatalf("channel count = %d", registry.len(testDeviceID))
	}
}

func newCatalogDevice(t *testing.T) (*sipgo.Client, <-chan *sip.Request) {
	t.Helper()
	ua, err := sipgo.NewUA(sipgo.WithUserAgent(testDeviceID), sipgo.WithUserAgentHostname("127.0.0.1"))
	if err != nil {
		t.Fatalf("NewUA() error = %v", err)
	}
	server, err := sipgo.NewServer(ua)
	if err != nil {
		_ = ua.Close()
		t.Fatalf("NewServer() error = %v", err)
	}
	queries := make(chan *sip.Request, 1)
	server.OnMessage(func(request *sip.Request, tx sip.ServerTransaction) {
		if err := tx.Respond(sip.NewResponseFromRequest(request, sip.StatusOK, "OK", nil)); err != nil {
			t.Errorf("Catalog query response error = %v", err)
		}
		queries <- request.Clone()
	})
	client, err := sipgo.NewClient(ua, sipgo.WithClientHostname("127.0.0.1"), sipgo.WithClientNAT())
	if err != nil {
		_ = ua.Close()
		t.Fatalf("NewClient() error = %v", err)
	}
	t.Cleanup(func() { _ = ua.Close() })
	return client, queries
}

func catalogXML(sn, sum int, channels []catalogChannel) []byte {
	return catalogResponseXML(testDeviceID, sn, sum, channels)
}

func catalogResponseXML(deviceID string, sn, sum int, channels []catalogChannel) []byte {
	var body bytes.Buffer
	fmt.Fprintf(&body, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n<Response><CmdType>Catalog</CmdType><SN>%d</SN><DeviceID>%s</DeviceID><SumNum>%d</SumNum><DeviceList Num=\"%d\">", sn, deviceID, sum, len(channels))
	for _, channel := range channels {
		fmt.Fprintf(&body, "<Item><DeviceID>%s</DeviceID><Name>%s</Name><ParentID>%s</ParentID><Status>%s</Status></Item>", channel.DeviceID, channel.Name, channel.ParentID, channel.Status)
	}
	body.WriteString("</DeviceList></Response>")
	return body.Bytes()
}
