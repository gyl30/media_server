package main

import (
	"fmt"
	"net"
	"net/url"
	"strconv"
	"strings"

	"github.com/pion/sdp/v3"
)

type liveSDPParameters struct {
	channelID   string
	mediaIP     string
	rtpPort     uint16
	payloadType uint8
	ssrc        uint32
}

type gbSession int

const (
	sessionPlay gbSession = iota
	sessionPlayback
	sessionDownload
)

func validDownloadSpeed(speed int) bool { return speed == 1 || speed == 2 || speed == 4 }

func buildGBSDP(parameters liveSDPParameters, startTime, endTime int64, session gbSession, speed int) ([]byte, error) {
	if session != sessionPlay && (startTime < 0 || endTime <= startTime) {
		return nil, fmt.Errorf("invalid playback SDP time range")
	}
	if session == sessionDownload && !validDownloadSpeed(speed) {
		return nil, fmt.Errorf("invalid download speed")
	}
	ip := net.ParseIP(parameters.mediaIP)
	if ip == nil || ip.IsUnspecified() || parameters.rtpPort == 0 || parameters.payloadType > 127 || parameters.ssrc == 0 || !validDigits(parameters.channelID, 20) {
		return nil, fmt.Errorf("invalid live SDP parameters")
	}
	addressType := "IP6"
	if ip.To4() != nil {
		addressType = "IP4"
	}
	description := sdp.SessionDescription{
		Version: 0,
		Origin: sdp.Origin{
			Username: parameters.channelID, NetworkType: "IN", AddressType: addressType, UnicastAddress: parameters.mediaIP,
		},
		SessionName: sdp.SessionName("Play"),
		ConnectionInformation: &sdp.ConnectionInformation{
			NetworkType: "IN", AddressType: addressType, Address: &sdp.Address{Address: parameters.mediaIP},
		},
		TimeDescriptions: []sdp.TimeDescription{{Timing: sdp.Timing{StartTime: 0, StopTime: 0}}},
		MediaDescriptions: []*sdp.MediaDescription{{
			MediaName: sdp.MediaName{
				Media: "video", Port: sdp.RangedPort{Value: int(parameters.rtpPort)}, Protos: []string{"RTP", "AVP"}, Formats: []string{strconv.Itoa(int(parameters.payloadType))},
			},
			Attributes: []sdp.Attribute{
				sdp.NewPropertyAttribute("recvonly"),
				sdp.NewAttribute("rtpmap", fmt.Sprintf("%d PS/90000", parameters.payloadType)),
			},
		}},
	}
	if session != sessionPlay {
		description.SessionName = "Playback"
		if session == sessionDownload {
			description.SessionName = "Download"
		}
		description.URI = &url.URL{Opaque: parameters.channelID + ":0"}
		description.TimeDescriptions[0].Timing = sdp.Timing{StartTime: uint64(startTime), StopTime: uint64(endTime)}
		media := description.MediaDescriptions[0]
		media.MediaName.Protos = []string{"TCP", "RTP", "AVP"}
		media.Attributes = append(media.Attributes, sdp.NewAttribute("setup", "passive"), sdp.NewAttribute("connection", "new"))
		if session == sessionDownload {
			media.Attributes = append(media.Attributes, sdp.NewAttribute("downloadspeed", strconv.Itoa(speed)))
		}
	}
	body, err := description.Marshal()
	if err != nil {
		return nil, err
	}
	body = append(body, fmt.Sprintf("y=%010d\r\nf=v/2/5/25/1/4000a/1/8/1\r\n", parameters.ssrc)...)
	return body, nil
}

func validateGBAnswer(body []byte, payloadType uint8, requestedSSRC uint32, playback bool) (uint32, error) {
	var standard strings.Builder
	ssrc := requestedSSRC
	seenY := false
	for _, line := range strings.Split(strings.ReplaceAll(string(body), "\r\n", "\n"), "\n") {
		line = strings.TrimSpace(line)
		switch {
		case strings.HasPrefix(line, "y="):
			if seenY {
				return 0, fmt.Errorf("duplicate SDP y field")
			}
			seenY = true
			value, err := strconv.ParseUint(strings.TrimPrefix(line, "y="), 10, 32)
			if err != nil || value == 0 {
				return 0, fmt.Errorf("invalid GB28181 SDP y field")
			}
			ssrc = uint32(value)
		case strings.HasPrefix(line, "f="):
		case playback && strings.HasPrefix(line, "u="):
		case line != "":
			standard.WriteString(line)
			standard.WriteString("\r\n")
		}
	}
	var description sdp.SessionDescription
	if err := description.UnmarshalString(standard.String()); err != nil {
		return 0, err
	}
	if len(description.MediaDescriptions) != 1 {
		return 0, fmt.Errorf("invalid live SDP session")
	}
	media := description.MediaDescriptions[0]
	protocol := "RTP/AVP"
	if playback {
		protocol = "TCP/RTP/AVP"
	}
	if media.MediaName.Media != "video" || media.MediaName.Port.Value <= 0 || strings.Join(media.MediaName.Protos, "/") != protocol ||
		len(media.MediaName.Formats) != 1 || media.MediaName.Formats[0] != strconv.Itoa(int(payloadType)) {
		return 0, fmt.Errorf("invalid live SDP media")
	}
	rtpmap, ok := media.Attribute("rtpmap")
	if !ok || !strings.EqualFold(rtpmap, fmt.Sprintf("%d PS/90000", payloadType)) {
		return 0, fmt.Errorf("invalid live SDP payload mapping")
	}
	direction := ""
	for _, attributes := range [][]sdp.Attribute{description.Attributes, media.Attributes} {
		for _, attribute := range attributes {
			switch attribute.Key {
			case "sendonly", "recvonly", "sendrecv", "inactive":
				direction = attribute.Key
			}
		}
	}
	if direction == "recvonly" || direction == "inactive" {
		return 0, fmt.Errorf("device SDP does not send media")
	}
	if playback {
		setup, hasSetup := media.Attribute("setup")
		connection, hasConnection := media.Attribute("connection")
		if !hasSetup {
			setup, _ = description.Attribute("setup")
		}
		if !hasConnection {
			connection, _ = description.Attribute("connection")
		}
		if setup != "active" || connection != "new" {
			return 0, fmt.Errorf("invalid playback SDP TCP negotiation")
		}
	}
	return ssrc, nil
}
