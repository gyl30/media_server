package main

import (
	"fmt"
	"net"
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

func buildLiveUDPSDP(parameters liveSDPParameters) ([]byte, error) {
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
	body, err := description.Marshal()
	if err != nil {
		return nil, err
	}
	body = append(body, fmt.Sprintf("y=%010d\r\nf=v/2/5/25/1/4000a/1/8/1\r\n", parameters.ssrc)...)
	return body, nil
}

func validateLiveUDPAnswer(body []byte, payloadType uint8, expectedSSRC uint32) error {
	var standard strings.Builder
	var y, format string
	seenY := false
	seenFormat := false
	for _, line := range strings.Split(strings.ReplaceAll(string(body), "\r\n", "\n"), "\n") {
		switch {
		case strings.HasPrefix(line, "y="):
			if seenY {
				return fmt.Errorf("duplicate SDP y field")
			}
			seenY = true
			y = strings.TrimPrefix(line, "y=")
		case strings.HasPrefix(line, "f="):
			if seenFormat {
				return fmt.Errorf("duplicate SDP f field")
			}
			seenFormat = true
			format = strings.TrimPrefix(line, "f=")
		case line != "":
			standard.WriteString(line)
			standard.WriteString("\r\n")
		}
	}
	if y != fmt.Sprintf("%010d", expectedSSRC) || format == "" {
		return fmt.Errorf("invalid GB28181 SDP fields")
	}
	var description sdp.SessionDescription
	if err := description.UnmarshalString(standard.String()); err != nil {
		return err
	}
	if description.Version != 0 || description.SessionName != "Play" || description.ConnectionInformation == nil ||
		description.ConnectionInformation.Address == nil || net.ParseIP(description.ConnectionInformation.Address.Address) == nil || len(description.TimeDescriptions) != 1 ||
		description.TimeDescriptions[0].Timing.StartTime != 0 || description.TimeDescriptions[0].Timing.StopTime != 0 || len(description.MediaDescriptions) != 1 {
		return fmt.Errorf("invalid live SDP session")
	}
	media := description.MediaDescriptions[0]
	if media.MediaName.Media != "video" || media.MediaName.Port.Value <= 0 || strings.Join(media.MediaName.Protos, "/") != "RTP/AVP" ||
		len(media.MediaName.Formats) != 1 || media.MediaName.Formats[0] != strconv.Itoa(int(payloadType)) {
		return fmt.Errorf("invalid live SDP media")
	}
	rtpmap, ok := media.Attribute("rtpmap")
	if !ok || !strings.EqualFold(rtpmap, fmt.Sprintf("%d PS/90000", payloadType)) {
		return fmt.Errorf("invalid live SDP payload mapping")
	}
	direction := ""
	for _, attribute := range description.Attributes {
		switch attribute.Key {
		case "sendonly", "recvonly", "sendrecv", "inactive":
			if direction != "" {
				return fmt.Errorf("multiple SDP direction attributes")
			}
			direction = attribute.Key
		}
	}
	mediaDirection := ""
	for _, attribute := range media.Attributes {
		switch attribute.Key {
		case "sendonly", "recvonly", "sendrecv", "inactive":
			if mediaDirection != "" {
				return fmt.Errorf("multiple SDP media direction attributes")
			}
			mediaDirection = attribute.Key
		}
	}
	if mediaDirection != "" {
		direction = mediaDirection
	}
	if direction != "sendonly" {
		return fmt.Errorf("device SDP is not sendonly")
	}
	return nil
}
