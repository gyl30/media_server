package main

import (
	"context"
	"encoding/xml"
	"fmt"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/emiago/sipgo/sip"
)

const alarmCapacity = 1000
const alarmPageLimit = 200

type alarmNotify struct {
	XMLName          xml.Name `xml:"Notify"`
	CmdType          string   `xml:"CmdType"`
	SN               int      `xml:"SN"`
	DeviceID         string   `xml:"DeviceID"`
	AlarmPriority    string   `xml:"AlarmPriority"`
	AlarmMethod      string   `xml:"AlarmMethod"`
	AlarmTime        string   `xml:"AlarmTime"`
	AlarmDescription string   `xml:"AlarmDescription"`
	Longitude        string   `xml:"Longitude"`
	Latitude         string   `xml:"Latitude"`
	AlarmType        string   `xml:"Info>AlarmType"`
}

type alarmResponse struct {
	XMLName  xml.Name `xml:"Response"`
	CmdType  string   `xml:"CmdType"`
	SN       int      `xml:"SN"`
	DeviceID string   `xml:"DeviceID"`
	Result   string   `xml:"Result"`
}

type alarmRecord struct {
	Seq         int64     `json:"seq"`
	DeviceID    string    `json:"device_id"`
	SourceID    string    `json:"source_id"`
	Priority    string    `json:"priority"`
	Method      string    `json:"method"`
	Type        string    `json:"type"`
	AlarmTime   string    `json:"alarm_time"`
	Description string    `json:"description"`
	Longitude   string    `json:"longitude"`
	Latitude    string    `json:"latitude"`
	ReceivedAt  time.Time `json:"received_at"`
}

type alarmLog struct {
	mu         sync.Mutex
	generation string
	records    []alarmRecord
	latest     int64
}

type alarmPage struct {
	Alarms     []alarmRecord `json:"alarms"`
	Latest     int64         `json:"latest"`
	NextCursor string        `json:"next_cursor"`
	Reset      bool          `json:"reset"`
	Gap        bool          `json:"gap"`
}

func (l *alarmLog) add(record alarmRecord) {
	l.mu.Lock()
	defer l.mu.Unlock()
	l.latest++
	record.Seq = l.latest
	l.records = append(l.records, record)
	if len(l.records) > alarmCapacity {
		l.records = append(l.records[:0:0], l.records[len(l.records)-alarmCapacity:]...)
	}
}

func (l *alarmLog) since(cursor string) (alarmPage, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	page := alarmPage{Alarms: []alarmRecord{}, Latest: l.latest}
	var after int64
	if cursor != "0" {
		generation, sequence, ok := strings.Cut(cursor, ".")
		var err error
		after, err = strconv.ParseInt(sequence, 10, 64)
		if !ok || generation == "" || err != nil || after < 0 || strconv.FormatInt(after, 10) != sequence {
			return alarmPage{}, fmt.Errorf("invalid alarm cursor")
		}
		if generation != l.generation || after > l.latest {
			page.Reset, after = true, 0
		}
	}
	for _, record := range l.records {
		if record.Seq <= after {
			continue
		}
		if len(page.Alarms) == 0 {
			page.Gap = record.Seq != after+1
		}
		if len(page.Alarms) == alarmPageLimit {
			break
		}
		page.Alarms = append(page.Alarms, record)
	}
	if len(page.Alarms) != 0 {
		after = page.Alarms[len(page.Alarms)-1].Seq
	}
	page.NextCursor = l.generation + "." + strconv.FormatInt(after, 10)
	return page, nil
}

func (s *sipServer) handleAlarm(req *sip.Request, tx sip.ServerTransaction) {
	var notify alarmNotify
	from := req.From()
	if err := decodeMANSCDP(req.Body(), &notify); err != nil || from == nil || notify.SN <= 0 || !validDigits(notify.DeviceID, 20) {
		s.respond(tx, sip.NewResponseFromRequest(req, sip.StatusBadRequest, "Bad Request", nil))
		return
	}
	deviceID := from.Address.User
	s.live.mu.Lock()
	_, stopping := s.live.stoppingDevices[deviceID]
	s.live.mu.Unlock()
	_, ownChannel := s.channels.get(deviceID, notify.DeviceID)
	if stopping || !s.devices.registeredSource(deviceID, req.Source(), s.now()) || (notify.DeviceID != deviceID && !ownChannel) {
		s.respond(tx, sip.NewResponseFromRequest(req, sip.StatusForbidden, "Forbidden", nil))
		return
	}
	s.respond(tx, sip.NewResponseFromRequest(req, sip.StatusOK, "OK", nil))
	s.alarms.add(alarmRecord{
		DeviceID: deviceID, SourceID: notify.DeviceID, Priority: notify.AlarmPriority, Method: notify.AlarmMethod, Type: notify.AlarmType,
		AlarmTime: notify.AlarmTime, Description: notify.AlarmDescription, Longitude: notify.Longitude, Latitude: notify.Latitude, ReceivedAt: s.now(),
	})
	device, ok := s.devices.get(deviceID)
	if !ok {
		return
	}
	body, err := xml.Marshal(alarmResponse{CmdType: "Alarm", SN: notify.SN, DeviceID: notify.DeviceID, Result: "OK"})
	if err != nil {
		return
	}
	ctx, cancel := context.WithTimeout(context.Background(), s.live.byeTimeout)
	defer cancel()
	if err := s.sendMANSCDP(ctx, device, append([]byte(xml.Header), body...)); err != nil {
		s.logger.Warn("alarm response failed", "device_id", deviceID, "sn", notify.SN, "error", err)
	}
}

func (s *infrastructureServer) handleAlarmList(writer http.ResponseWriter, request *http.Request) {
	page, err := s.live.sip.alarms.since(request.PathValue("cursor"))
	if err != nil {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	writeJSON(writer, http.StatusOK, page)
}
