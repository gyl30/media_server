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
}

type devicePreset struct {
	ID   string `xml:"PresetID" json:"preset_id"`
	Name string `xml:"PresetName" json:"preset_name"`
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
	pending := pendingDeviceQuery{deviceID: deviceID, targetID: targetID, command: command.Command, response: make(chan deviceQueryResponse, 1)}
	sipServer.pendingQueries[sn] = pending
	s.live.mu.Unlock()
	defer func() {
		s.live.mu.Lock()
		delete(sipServer.pendingQueries, sn)
		s.live.mu.Unlock()
	}()
	ctx, cancel := context.WithTimeout(request.Context(), 5*time.Second)
	defer cancel()
	body, err := xml.Marshal(manscdpQuery{CmdType: command.Command, SN: sn, DeviceID: targetID})
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
	s.logger.Warn("Device query failed", "device_id", deviceID, "command", command.Command, "error", err)
	writeHTTPError(writer, http.StatusBadGateway, "device_query_failed")
}

func (s *sipServer) handleDeviceQueryResponse(req *sip.Request, tx sip.ServerTransaction) {
	var message struct {
		XMLName    xml.Name `xml:"Response"`
		CmdType    string   `xml:"CmdType"`
		SN         int      `xml:"SN"`
		DeviceID   string   `xml:"DeviceID"`
		Result     string   `xml:"Result"`
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
	if message.CmdType == "PresetQuery" {
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
	if from == nil || (message.CmdType != "PresetQuery" && from.Address.User != message.DeviceID) {
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
	if ok && message.CmdType == "PresetQuery" && pending.deviceID != from.Address.User {
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
		} else {
			response.Result = ""
			response.ChannelID = pending.targetID
			presets := append([]devicePreset{}, message.PresetList.Items...)
			response.Presets = &presets
		}
		select {
		case pending.response <- response:
		default:
		}
	}
	s.live.mu.Unlock()
	s.respond(tx, sip.NewResponseFromRequest(req, sip.StatusOK, "OK", nil))
}
