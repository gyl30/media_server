package main

import (
	"context"
	"encoding/xml"
	"fmt"
	"net/http"
	"time"
)

type ptzControl struct {
	XMLName    xml.Name `xml:"Control"`
	Command    string   `xml:"CmdType"`
	SN         int      `xml:"SN"`
	DeviceID   string   `xml:"DeviceID"`
	PTZCommand string   `xml:"PTZCmd"`
	Info       struct {
		Priority int `xml:"ControlPriority"`
	} `xml:"Info"`
}

type ptzResponse struct {
	DeviceID       string `json:"device_id"`
	ChannelID      string `json:"channel_id"`
	Command        string `json:"command"`
	Start          string `json:"start,omitempty"`
	Stop           string `json:"stop"`
	PhysicalResult string `json:"physical_result"`
}

func encodePTZ(command string, speed int) (string, error) {
	packet := [8]byte{0xA5, 0x0F}
	switch command {
	case "right":
		packet[3] = 0x01
	case "left":
		packet[3] = 0x02
	case "down":
		packet[3] = 0x04
	case "up":
		packet[3] = 0x08
	case "down_right":
		packet[3] = 0x05
	case "down_left":
		packet[3] = 0x06
	case "up_right":
		packet[3] = 0x09
	case "up_left":
		packet[3] = 0x0A
	case "zoom_in":
		packet[3] = 0x10
	case "zoom_out":
		packet[3] = 0x20
	case "stop":
		if speed != 0 {
			return "", fmt.Errorf("STOP does not accept speed")
		}
	default:
		return "", fmt.Errorf("unsupported PTZ command")
	}
	if packet[3]&0x0F != 0 {
		if speed < 1 || speed > 255 {
			return "", fmt.Errorf("invalid direction speed")
		}
		if packet[3]&0x03 != 0 {
			packet[4] = byte(speed)
		}
		if packet[3]&0x0C != 0 {
			packet[5] = byte(speed)
		}
	} else if command != "stop" {
		if speed < 1 || speed > 15 {
			return "", fmt.Errorf("invalid zoom speed")
		}
		packet[6] = byte(speed << 4)
	}
	for _, value := range packet[:7] {
		packet[7] += value
	}
	return fmt.Sprintf("%X", packet), nil
}

func (s *infrastructureServer) handlePTZ(writer http.ResponseWriter, request *http.Request) {
	deviceID, channelID := request.PathValue("device_id"), request.PathValue("channel_id")
	var command struct {
		Command string `json:"command"`
		Speed   int    `json:"speed"`
	}
	if !validDigits(deviceID, 20) || !validDigits(channelID, 20) || !decodeJSON(writer, request, &command) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	encoded, err := encodePTZ(command.Command, command.Speed)
	if err != nil {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	key := liveKey{deviceID: deviceID, channelID: channelID}
	s.live.mu.Lock()
	if cancel, exists := s.live.ptzOwners[key]; exists {
		s.live.mu.Unlock()
		if command.Command != "stop" {
			writeHTTPError(writer, http.StatusConflict, "ptz_busy")
			return
		}
		cancel()
		writeJSON(writer, http.StatusAccepted, ptzResponse{DeviceID: deviceID, ChannelID: channelID, Command: "stop", Stop: "requested", PhysicalResult: "unconfirmed"})
		return
	}
	if _, err := s.live.sip.deviceStore.get(request.Context(), deviceID); err != nil {
		s.live.mu.Unlock()
		s.writeDeviceError(writer, err)
		return
	}
	if _, stopping := s.live.stoppingDevices[deviceID]; stopping {
		s.live.mu.Unlock()
		writeHTTPError(writer, http.StatusConflict, "device_stopping")
		return
	}
	device, online := s.live.sip.devices.getOnline(deviceID, s.live.sip.now())
	if !online {
		s.live.mu.Unlock()
		writeHTTPError(writer, http.StatusConflict, "device_offline")
		return
	}
	if _, exists := s.live.sip.channels.get(deviceID, channelID); !exists {
		s.live.mu.Unlock()
		writeHTTPError(writer, http.StatusNotFound, "channel_not_found")
		return
	}
	// cancel 只控制 START/点动等待；STOP 必须使用独立 context，连续 STOP 不得取消它。
	actionContext, cancel := context.WithCancel(request.Context())
	s.live.ptzOwners[key] = cancel
	s.live.mu.Unlock()
	defer func() { cancel(); s.live.mu.Lock(); delete(s.live.ptzOwners, key); s.live.mu.Unlock() }()
	control := ptzControl{Command: "DeviceControl", DeviceID: channelID, PTZCommand: encoded}
	control.Info.Priority = 5
	var startError error
	if command.Command != "stop" {
		control.SN = int(s.live.sip.messageSN.Add(1))
		body, err := xml.Marshal(control)
		startError = err
		if startError == nil {
			startContext, stopStart := context.WithTimeout(actionContext, 300*time.Millisecond)
			startError = startContext.Err()
			if startError == nil {
				startError = s.live.sip.sendMANSCDP(startContext, device, append([]byte(xml.Header), body...))
			}
			if startError == nil {
				<-startContext.Done()
			}
			stopStart()
		}
	}
	// 等 START 的 Do 返回、Terminate 完成本地重传终止后，再尝试原目标 STOP。
	control.SN = int(s.live.sip.messageSN.Add(1))
	control.PTZCommand = "A50F0000000000B4"
	body, stopError := xml.Marshal(control)
	if stopError == nil {
		stopContext, stopStop := context.WithTimeout(context.Background(), 1500*time.Millisecond)
		stopError = s.live.sip.sendMANSCDP(stopContext, device, append([]byte(xml.Header), body...))
		stopStop()
	}
	if startError != nil {
		s.logger.Warn("PTZ START failed", "device_id", deviceID, "channel_id", channelID, "error", startError)
	}
	if stopError != nil {
		s.logger.Warn("PTZ STOP unconfirmed", "device_id", deviceID, "channel_id", channelID, "error", stopError)
		writeHTTPError(writer, http.StatusBadGateway, "ptz_stop_unconfirmed")
		return
	}
	if startError != nil {
		writeHTTPError(writer, http.StatusBadGateway, "ptz_start_failed")
		return
	}
	response := ptzResponse{DeviceID: deviceID, ChannelID: channelID, Command: command.Command, Stop: "accepted", PhysicalResult: "unconfirmed"}
	if command.Command != "stop" {
		response.Start = "accepted"
	}
	writeJSON(writer, http.StatusOK, response)
}
