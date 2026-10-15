package main

import (
	"context"
	"encoding/xml"
	"errors"
	"net/http"
	"time"

	"github.com/emiago/sipgo/sip"
)

type pendingDeviceQuery struct {
	deviceID string
	targetID string
	command  string
	response chan deviceQueryResponse
	expected int
	records  []deviceRecord
}

type deviceInfo struct {
	DeviceName   string `xml:"DeviceName" json:"device_name"`
	Manufacturer string `xml:"Manufacturer" json:"manufacturer"`
	Model        string `xml:"Model" json:"model"`
	Firmware     string `xml:"Firmware" json:"firmware"`
	Channel      *int   `xml:"Channel" json:"channel,omitempty"`
}

type deviceStatus struct {
	Online     string `xml:"Online" json:"online"`
	Status     string `xml:"Status" json:"status"`
	DeviceTime string `xml:"DeviceTime" json:"device_time"`
	Encode     string `xml:"Encode" json:"encode"`
	Record     string `xml:"Record" json:"record"`
	Reason     string `xml:"Reason" json:"reason"`
}

type deviceQueryResponse struct {
	DeviceID  string          `json:"device_id"`
	ChannelID string          `json:"channel_id,omitempty"`
	Command   string          `json:"command"`
	SN        int             `json:"sn"`
	Result    string          `json:"result,omitempty"`
	Info      *deviceInfo     `json:"info,omitempty"`
	Status    *deviceStatus   `json:"status,omitempty"`
	Presets   *[]devicePreset `json:"presets,omitempty"`
	SumNum    *int            `json:"sum_num,omitempty"`
	Records   *[]deviceRecord `json:"records,omitempty"`
	failure   string          `json:"-"`
}

type devicePreset struct {
	ID   string `xml:"PresetID" json:"preset_id"`
	Name string `xml:"PresetName" json:"preset_name"`
}

type deviceRecord struct {
	DeviceID   string  `xml:"DeviceID" json:"device_id"`
	Name       *string `xml:"Name" json:"name"`
	FilePath   string  `xml:"FilePath" json:"file_path"`
	Address    string  `xml:"Address" json:"address"`
	StartTime  string  `xml:"StartTime" json:"start_time"`
	EndTime    string  `xml:"EndTime" json:"end_time"`
	Secrecy    string  `xml:"Secrecy" json:"secrecy"`
	Type       string  `xml:"Type" json:"type"`
	RecorderID string  `xml:"RecorderID" json:"recorder_id"`
	FileSize   string  `xml:"FileSize" json:"file_size"`
}

func (s *infrastructureServer) handleDeviceQuery(writer http.ResponseWriter, request *http.Request) {
	deviceID := request.PathValue("device_id")
	channelID := request.PathValue("channel_id")
	var command struct {
		Command string `json:"command"`
	}
	if channelID != "" {
		command.Command = "PresetQuery"
	}
	if !validDigits(deviceID, 20) || (channelID != "" && !validDigits(channelID, 20)) ||
		(channelID == "" && (!decodeJSON(writer, request, &command) || (command.Command != "DeviceInfo" && command.Command != "DeviceStatus"))) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	s.runDeviceQuery(writer, request, manscdpQuery{CmdType: command.Command})
}

func (s *infrastructureServer) handleRecordQuery(writer http.ResponseWriter, request *http.Request) {
	var query struct {
		StartTime string `json:"start_time"`
		EndTime   string `json:"end_time"`
		Type      string `json:"type"`
	}
	if !validDigits(request.PathValue("device_id"), 20) || !validDigits(request.PathValue("channel_id"), 20) || !decodeJSON(writer, request, &query) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	const layout = "2006-01-02T15:04:05"
	start, startError := time.Parse(layout, query.StartTime)
	end, endError := time.Parse(layout, query.EndTime)
	if startError != nil || endError != nil || start.Format(layout) != query.StartTime || end.Format(layout) != query.EndTime || !start.Before(end) ||
		(query.Type != "time" && query.Type != "alarm" && query.Type != "manual" && query.Type != "all") {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	s.runDeviceQuery(writer, request, manscdpQuery{CmdType: "RecordInfo", StartTime: query.StartTime, EndTime: query.EndTime, Type: query.Type})
}

func (s *infrastructureServer) runDeviceQuery(writer http.ResponseWriter, request *http.Request, query manscdpQuery) {
	deviceID := request.PathValue("device_id")
	channelID := request.PathValue("channel_id")
	if _, err := s.live.sip.deviceStore.get(request.Context(), deviceID); err != nil {
		s.writeDeviceError(writer, err)
		return
	}
	sipServer := s.live.sip
	s.live.mu.Lock()
	if _, stopping := s.live.stoppingDevices[deviceID]; stopping {
		s.live.mu.Unlock()
		writeHTTPError(writer, http.StatusConflict, "device_stopping")
		return
	}
	device, online := sipServer.devices.getOnline(deviceID, sipServer.now())
	if !online {
		s.live.mu.Unlock()
		writeHTTPError(writer, http.StatusConflict, "device_offline")
		return
	}
	if channelID != "" {
		if _, exists := sipServer.channels.get(deviceID, channelID); !exists {
			s.live.mu.Unlock()
			writeHTTPError(writer, http.StatusNotFound, "channel_not_found")
			return
		}
	}
	sn := int(sipServer.messageSN.Add(1))
	targetID := deviceID
	if channelID != "" {
		targetID = channelID
	}
	pending := pendingDeviceQuery{deviceID: deviceID, targetID: targetID, command: query.CmdType, response: make(chan deviceQueryResponse, 1)}
	sipServer.pendingQueries[sn] = pending
	s.live.mu.Unlock()
	defer func() {
		s.live.mu.Lock()
		delete(sipServer.pendingQueries, sn)
		s.live.mu.Unlock()
	}()
	timeout := 5 * time.Second
	if query.CmdType == "RecordInfo" {
		timeout = 15 * time.Second
	}
	ctx, cancel := context.WithTimeout(request.Context(), timeout)
	defer cancel()
	query.SN, query.DeviceID = sn, targetID
	body, err := xml.Marshal(query)
	if err == nil {
		err = sipServer.sendMANSCDP(ctx, device, append([]byte(xml.Header), body...))
	}
	if err == nil {
		select {
		case response := <-pending.response:
			if ctx.Err() != nil {
				err = ctx.Err()
				break
			}
			if response.failure != "" {
				writeHTTPError(writer, http.StatusUnprocessableEntity, response.failure)
				return
			}
			writeJSON(writer, http.StatusOK, response)
			return
		case <-ctx.Done():
			err = ctx.Err()
		}
	}
	if errors.Is(ctx.Err(), context.DeadlineExceeded) {
		writeHTTPError(writer, http.StatusGatewayTimeout, "device_query_timeout")
		return
	}
	s.logger.Warn("Device query failed", "device_id", deviceID, "command", query.CmdType, "error", err)
	writeHTTPError(writer, http.StatusBadGateway, "device_query_failed")
}

func (s *sipServer) handleDeviceQueryResponse(req *sip.Request, tx sip.ServerTransaction) {
	var message struct {
		XMLName    xml.Name `xml:"Response"`
		CmdType    string   `xml:"CmdType"`
		SN         int      `xml:"SN"`
		DeviceID   string   `xml:"DeviceID"`
		Result     string   `xml:"Result"`
		Name       *string  `xml:"Name"`
		SumNum     *int     `xml:"SumNum"`
		RecordList *struct {
			Num   *int           `xml:"Num,attr"`
			Items []deviceRecord `xml:"Item"`
		} `xml:"RecordList"`
		PresetList *struct {
			Num   *int           `xml:"Num,attr"`
			Items []devicePreset `xml:"Item"`
		} `xml:"PresetList"`
		deviceInfo
		deviceStatus
	}
	if err := decodeMANSCDP(req.Body(), &message); err != nil {
		s.respond(tx, sip.NewResponseFromRequest(req, sip.StatusBadRequest, "Bad Request", nil))
		return
	}
	if message.CmdType == "RecordInfo" {
		if message.Result != "" || message.Name == nil || message.SumNum == nil || *message.SumNum < 0 ||
			(message.RecordList == nil && *message.SumNum != 0) ||
			(message.RecordList != nil && (message.RecordList.Num == nil || *message.RecordList.Num < 0 || *message.RecordList.Num != len(message.RecordList.Items) || len(message.RecordList.Items) > *message.SumNum)) {
			s.respond(tx, sip.NewResponseFromRequest(req, sip.StatusBadRequest, "Bad Request", nil))
			return
		}
		if message.RecordList != nil {
			for index := range message.RecordList.Items {
				record := &message.RecordList.Items[index]
				if record.DeviceID != message.DeviceID || record.Name == nil {
					s.respond(tx, sip.NewResponseFromRequest(req, sip.StatusBadRequest, "Bad Request", nil))
					return
				}
				if record.Secrecy == "" {
					record.Secrecy = "0"
				}
			}
		}
	} else if message.CmdType == "PresetQuery" {
		if message.PresetList == nil || message.PresetList.Num == nil || *message.PresetList.Num != len(message.PresetList.Items) {
			s.respond(tx, sip.NewResponseFromRequest(req, sip.StatusBadRequest, "Bad Request", nil))
			return
		}
		seen := make(map[string]struct{}, len(message.PresetList.Items))
		for _, preset := range message.PresetList.Items {
			if _, duplicate := seen[preset.ID]; preset.ID == "" || duplicate {
				s.respond(tx, sip.NewResponseFromRequest(req, sip.StatusBadRequest, "Bad Request", nil))
				return
			}
			seen[preset.ID] = struct{}{}
		}
	} else if message.Result != "OK" && message.Result != "ERROR" {
		s.respond(tx, sip.NewResponseFromRequest(req, sip.StatusBadRequest, "Bad Request", nil))
		return
	}
	from := req.From()
	if from == nil || ((message.CmdType != "PresetQuery" && message.CmdType != "RecordInfo") && from.Address.User != message.DeviceID) {
		s.respond(tx, sip.NewResponseFromRequest(req, sip.StatusBadRequest, "Bad Request", nil))
		return
	}
	if message.Result == "OK" && ((message.CmdType == "DeviceInfo" && message.Channel != nil && *message.Channel < 0) ||
		(message.CmdType == "DeviceStatus" && ((message.Online != "ONLINE" && message.Online != "OFFLINE") || (message.Status != "OK" && message.Status != "ERROR")))) {
		s.respond(tx, sip.NewResponseFromRequest(req, sip.StatusBadRequest, "Bad Request", nil))
		return
	}
	s.live.mu.Lock()
	_, stopping := s.live.stoppingDevices[from.Address.User]
	if stopping || !s.devices.registeredSource(from.Address.User, req.Source(), s.now()) {
		s.live.mu.Unlock()
		s.respond(tx, sip.NewResponseFromRequest(req, sip.StatusForbidden, "Forbidden", nil))
		return
	}
	pending, ok := s.pendingQueries[message.SN]
	if ok && (message.CmdType == "PresetQuery" || message.CmdType == "RecordInfo") && pending.deviceID != from.Address.User {
		s.live.mu.Unlock()
		s.respond(tx, sip.NewResponseFromRequest(req, sip.StatusForbidden, "Forbidden", nil))
		return
	}
	if ok && pending.deviceID == from.Address.User && pending.targetID == message.DeviceID && pending.command == message.CmdType {
		response := deviceQueryResponse{DeviceID: pending.deviceID, Command: message.CmdType, SN: message.SN, Result: message.Result}
		if message.CmdType == "DeviceInfo" {
			response.Info = &message.deviceInfo
		} else if message.CmdType == "DeviceStatus" {
			response.Status = &message.deviceStatus
		} else if message.CmdType == "PresetQuery" {
			response.Result = ""
			response.ChannelID = pending.targetID
			presets := append([]devicePreset{}, message.PresetList.Items...)
			response.Presets = &presets
		} else {
			var items []deviceRecord
			if message.RecordList != nil {
				items = message.RecordList.Items
			}
			if (pending.records != nil && pending.expected != *message.SumNum) || len(pending.records)+len(items) > *message.SumNum {
				s.live.mu.Unlock()
				s.respond(tx, sip.NewResponseFromRequest(req, sip.StatusBadRequest, "Bad Request", nil))
				return
			}
			if *message.SumNum > 1000 {
				response.failure = "record_result_too_large"
			} else {
				if pending.records == nil {
					pending.records = []deviceRecord{}
					pending.expected = *message.SumNum
				}
				pending.records = append(pending.records, items...)
				s.pendingQueries[message.SN] = pending
				if len(pending.records) != pending.expected {
					s.live.mu.Unlock()
					s.respond(tx, sip.NewResponseFromRequest(req, sip.StatusOK, "OK", nil))
					return
				}
				records := append([]deviceRecord{}, pending.records...)
				response.ChannelID = pending.targetID
				response.SumNum = &pending.expected
				response.Records = &records
			}
		}
		select {
		case pending.response <- response:
		default:
		}
	}
	s.live.mu.Unlock()
	s.respond(tx, sip.NewResponseFromRequest(req, sip.StatusOK, "OK", nil))
}
