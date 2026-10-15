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
	DeviceID string        `json:"device_id"`
	Command  string        `json:"command"`
	SN       int           `json:"sn"`
	Result   string        `json:"result"`
	Info     *deviceInfo   `json:"info,omitempty"`
	Status   *deviceStatus `json:"status,omitempty"`
}

func (s *infrastructureServer) handleDeviceQuery(writer http.ResponseWriter, request *http.Request) {
	deviceID := request.PathValue("device_id")
	var command struct {
		Command string `json:"command"`
	}
	if !validDigits(deviceID, 20) || !decodeJSON(writer, request, &command) || (command.Command != "DeviceInfo" && command.Command != "DeviceStatus") {
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
	sn := int(sipServer.messageSN.Add(1))
	pending := pendingDeviceQuery{deviceID: deviceID, command: command.Command, response: make(chan deviceQueryResponse, 1)}
	sipServer.pendingQueries[sn] = pending
	s.live.mu.Unlock()
	defer func() {
		s.live.mu.Lock()
		delete(sipServer.pendingQueries, sn)
		s.live.mu.Unlock()
	}()
	ctx, cancel := context.WithTimeout(request.Context(), 5*time.Second)
	defer cancel()
	body, err := xml.Marshal(manscdpQuery{CmdType: command.Command, SN: sn, DeviceID: deviceID})
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
		XMLName  xml.Name `xml:"Response"`
		CmdType  string   `xml:"CmdType"`
		SN       int      `xml:"SN"`
		DeviceID string   `xml:"DeviceID"`
		Result   string   `xml:"Result"`
		deviceInfo
		deviceStatus
	}
	if err := decodeMANSCDP(req.Body(), &message); err != nil || (message.Result != "OK" && message.Result != "ERROR") {
		s.respond(tx, sip.NewResponseFromRequest(req, sip.StatusBadRequest, "Bad Request", nil))
		return
	}
	from := req.From()
	if from == nil || from.Address.User != message.DeviceID {
		s.respond(tx, sip.NewResponseFromRequest(req, sip.StatusBadRequest, "Bad Request", nil))
		return
	}
	if message.Result == "OK" && ((message.CmdType == "DeviceInfo" && message.Channel != nil && *message.Channel < 0) ||
		(message.CmdType == "DeviceStatus" && ((message.Online != "ONLINE" && message.Online != "OFFLINE") || (message.Status != "OK" && message.Status != "ERROR")))) {
		s.respond(tx, sip.NewResponseFromRequest(req, sip.StatusBadRequest, "Bad Request", nil))
		return
	}
	s.live.mu.Lock()
	_, stopping := s.live.stoppingDevices[message.DeviceID]
	if stopping || !s.devices.registeredSource(message.DeviceID, req.Source(), s.now()) {
		s.live.mu.Unlock()
		s.respond(tx, sip.NewResponseFromRequest(req, sip.StatusForbidden, "Forbidden", nil))
		return
	}
	pending, ok := s.pendingQueries[message.SN]
	if ok && pending.deviceID == message.DeviceID && pending.command == message.CmdType {
		response := deviceQueryResponse{DeviceID: message.DeviceID, Command: message.CmdType, SN: message.SN, Result: message.Result}
		if message.CmdType == "DeviceInfo" {
			response.Info = &message.deviceInfo
		} else {
			response.Status = &message.deviceStatus
		}
		select {
		case pending.response <- response:
		default:
		}
	}
	s.live.mu.Unlock()
	s.respond(tx, sip.NewResponseFromRequest(req, sip.StatusOK, "OK", nil))
}
