package main

import (
	"encoding/json"
	"encoding/xml"
	"fmt"
	"net/http"
	"strconv"
	"sync"
	"testing"
	"time"

	"github.com/emiago/sipgo/sip"
)

func TestAlarmLogRestartCatchesUp(t *testing.T) {
	for _, count := range []int{100, 150} {
		t.Run(strconv.Itoa(count), func(t *testing.T) {
			// 旧平台的客户端已经读取到 100；重启后的新平台在下次轮询前追平或超过它。
			old := &alarmLog{generation: "old"}
			for range 100 {
				old.add(alarmRecord{})
			}
			previous, err := old.since("0")
			if err != nil {
				t.Fatal(err)
			}
			log := &alarmLog{generation: "new"}
			for range count {
				log.add(alarmRecord{})
			}
			page, err := log.since(previous.NextCursor)
			if err != nil || !page.Reset || page.Gap || len(page.Alarms) != count || page.Alarms[0].Seq != 1 || page.NextCursor != "new."+strconv.Itoa(count) {
				t.Fatalf("restarted log lost alarms: count=%d page=%+v error=%v", count, page, err)
			}
		})
	}
}

func TestAlarmLogPagingResetAndGap(t *testing.T) {
	log := &alarmLog{generation: "current"}
	for index := range 300 {
		log.add(alarmRecord{SourceID: strconv.Itoa(index)})
	}
	// 只推进到实际返回的最后一条，不能跳到服务端最大序号。
	page, err := log.since("0")
	if err != nil || len(page.Alarms) != alarmPageLimit || page.Alarms[0].Seq != 1 || page.Alarms[len(page.Alarms)-1].Seq != 200 || page.Latest != 300 || page.Reset || page.Gap || page.NextCursor != "current.200" {
		t.Fatalf("first page=%+v error=%v", page, err)
	}
	page, err = log.since(page.NextCursor)
	if err != nil || len(page.Alarms) != 100 || page.Alarms[0].Seq != 201 || page.Reset || page.Gap || page.NextCursor != "current.300" {
		t.Fatalf("second page=%+v error=%v", page, err)
	}
	if page, err = log.since(page.NextCursor); err != nil || len(page.Alarms) != 0 || page.Reset || page.Gap || page.NextCursor != "current.300" {
		t.Fatalf("caught up page=%+v error=%v", page, err)
	}
	// 同代超前游标也显式重置，避免客户端永远等不到新数据。
	if page, err = log.since("current.800"); err != nil || !page.Reset || len(page.Alarms) == 0 || page.Alarms[0].Seq != 1 {
		t.Fatalf("stale cursor page=%+v error=%v", page, err)
	}
	for range alarmCapacity {
		log.add(alarmRecord{})
	}
	// 未读记录被容量淘汰时提示缺口。
	page, err = log.since("current.10")
	if err != nil || !page.Gap || page.Alarms[0].Seq != page.Latest-alarmCapacity+1 || len(log.records) != alarmCapacity {
		t.Fatalf("evicted page=%+v error=%v", page, err)
	}
}

func TestAlarmLogCursorValidationAndEmptyRestart(t *testing.T) {
	log := &alarmLog{generation: "current"}
	for _, cursor := range []string{"", "-1", "1", "abc", ".0", "current.", "current.-1", "current.+1", "current.01", "current.1.2", "current.9223372036854775808"} {
		if _, err := log.since(cursor); err == nil {
			t.Errorf("invalid cursor accepted: %q", cursor)
		}
	}
	page, err := log.since("0")
	if err != nil || page.NextCursor != "current.0" || page.Reset || page.Gap || len(page.Alarms) != 0 {
		t.Fatalf("empty page=%+v error=%v", page, err)
	}
	restarted := &alarmLog{generation: "new"}
	page, err = restarted.since(page.NextCursor)
	if err != nil || page.NextCursor != "new.0" || !page.Reset || page.Gap {
		t.Fatalf("empty restart page=%+v error=%v", page, err)
	}
}

func TestAlarmLogConcurrentReadersAndWriters(t *testing.T) {
	log := &alarmLog{generation: "current"}
	var writers sync.WaitGroup
	for range 4 {
		writers.Go(func() {
			cursor := "0"
			for range 300 {
				log.add(alarmRecord{})
				page, err := log.since(cursor)
				if err != nil || page.Reset || len(page.Alarms) > alarmPageLimit {
					t.Errorf("concurrent page=%+v error=%v", page, err)
					return
				}
				cursor = page.NextCursor
			}
		})
	}
	writers.Wait()
	if log.latest != 1200 || len(log.records) != alarmCapacity {
		t.Fatalf("concurrent log latest=%d records=%d", log.latest, len(log.records))
	}
}

// 真实 UDP SIP：父设备上报自身或已知通道报警，平台回 200 后发送同 SN 的报警通知响应。
func TestAlarmNotifyResponseAndList(t *testing.T) {
	s := testInfrastructure(t)
	var mu sync.Mutex
	var responses []alarmResponse
	peer := newDeviceMessagePeer(t, s, func(request *sip.Request, transaction sip.ServerTransaction, _ *deviceQueryPeer) {
		var response alarmResponse
		if err := xml.Unmarshal(request.Body(), &response); err != nil {
			t.Error(err)
		}
		mu.Lock()
		responses = append(responses, response)
		mu.Unlock()
		_ = transaction.Respond(sip.NewResponseFromRequest(request, sip.StatusOK, "OK", nil))
	})
	addPresetQueryChannels(t, s, presetQueryChannelID)
	registered, _ := s.live.sip.devices.get(queryTestDeviceID)
	registered.lastHeartbeat = s.live.sip.now().Add(-time.Minute)
	s.live.sip.devices.register(registered)
	notify := func(sn int, source, description string) []byte {
		return []byte(fmt.Sprintf(`<?xml version="1.0" encoding="UTF-8"?><Notify><CmdType>Alarm</CmdType><SN>%d</SN><DeviceID>%s</DeviceID>`+
			`<AlarmPriority>1</AlarmPriority><AlarmMethod>5</AlarmMethod><AlarmTime>2026-10-16T12:00:00</AlarmTime>`+
			`<AlarmDescription>%s</AlarmDescription><Info><AlarmType>2</AlarmType></Info></Notify>`, sn, source, description))
	}
	peer.send(t, notify(11, presetQueryChannelID, "通道移动侦测"), queryTestDeviceID, http.StatusOK)
	peer.send(t, notify(12, queryTestDeviceID, "设备防拆"), queryTestDeviceID, http.StatusOK)
	current, _ := s.live.sip.devices.get(queryTestDeviceID)
	if !current.lastHeartbeat.Equal(registered.lastHeartbeat) {
		t.Fatal("alarm notification refreshed heartbeat")
	}
	// 非所属通道、未注册设备与坏报文不记录、不回响应。
	peer.send(t, notify(13, "34020000001310009999", "外来通道"), queryTestDeviceID, http.StatusForbidden)
	peer.send(t, notify(14, "34020000001310009999", "未注册设备"), "34020000001310009999", http.StatusForbidden)
	peer.send(t, []byte(`<Notify><CmdType>Alarm</CmdType><SN>0</SN><DeviceID>bad</DeviceID></Notify>`), queryTestDeviceID, http.StatusBadRequest)
	for _, reason := range []string{"wrong endpoint", "offline", "expired", "stopping"} {
		device := registered
		switch reason {
		case "wrong endpoint":
			device.remoteEndpoint = "127.0.0.1:1"
		case "offline":
			device.online = false
		case "expired":
			device.expiresAt = s.live.sip.now()
		case "stopping":
			s.live.mu.Lock()
			s.live.stoppingDevices[queryTestDeviceID] = struct{}{}
			s.live.mu.Unlock()
		}
		s.live.sip.devices.register(device)
		peer.send(t, notify(15, presetQueryChannelID, reason), queryTestDeviceID, http.StatusForbidden)
		s.live.sip.devices.register(registered)
		s.live.mu.Lock()
		delete(s.live.stoppingDevices, queryTestDeviceID)
		s.live.mu.Unlock()
	}
	deadline := time.Now().Add(2 * time.Second)
	for {
		mu.Lock()
		count := len(responses)
		mu.Unlock()
		if count == 2 || time.Now().After(deadline) {
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	mu.Lock()
	if len(responses) != 2 || responses[0].SN != 11 || responses[0].DeviceID != presetQueryChannelID || responses[0].Result != "OK" || responses[0].CmdType != "Alarm" ||
		responses[1].SN != 12 || responses[1].DeviceID != queryTestDeviceID {
		t.Fatalf("alarm responses %+v", responses)
	}
	mu.Unlock()
	var listed struct {
		Alarms     []alarmRecord `json:"alarms"`
		Latest     int64         `json:"latest"`
		Reset      bool          `json:"reset"`
		Gap        bool          `json:"gap"`
		NextCursor string        `json:"next_cursor"`
	}
	if err := json.Unmarshal(apiRequest(t, s.handler(), "GET", "/api/alarms/since/0", "", http.StatusOK).Body.Bytes(), &listed); err != nil {
		t.Fatal(err)
	}
	if len(listed.Alarms) != 2 || listed.Latest != 2 || listed.Reset || listed.Gap || listed.Alarms[0].DeviceID != queryTestDeviceID ||
		listed.Alarms[0].SourceID != presetQueryChannelID || listed.Alarms[0].Description != "通道移动侦测" || listed.Alarms[0].Type != "2" ||
		listed.Alarms[0].Priority != "1" || listed.Alarms[0].Method != "5" || listed.Alarms[1].Seq != 2 || listed.NextCursor != s.live.sip.alarms.generation+".2" {
		t.Fatalf("alarm list %+v", listed)
	}
	apiRequest(t, s.handler(), "GET", "/api/alarms/since/-1", "", http.StatusBadRequest)
	apiRequest(t, s.handler(), "GET", "/api/alarms/since/abc", "", http.StatusBadRequest)
	apiRequest(t, s.handler(), "GET", "/api/alarms/since/"+listed.NextCursor, "", http.StatusOK)
}
