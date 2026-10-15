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

func buildGBSDP(parameters liveSDPParameters, startTime, endTime int64, kind receiverKind) ([]byte, error) {
	if kind == playbackReceiver && (startTime < 0 || endTime <= startTime) {
		return nil, fmt.Errorf("invalid playback SDP time range")
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
	if kind == playbackReceiver {
		description.SessionName = "Playback"
		description.URI = &url.URL{Opaque: parameters.channelID + ":0"}
		description.TimeDescriptions[0].Timing = sdp.Timing{StartTime: uint64(startTime), StopTime: uint64(endTime)}
		media := description.MediaDescriptions[0]
		media.MediaName.Protos = []string{"TCP", "RTP", "AVP"}
		media.Attributes = append(media.Attributes, sdp.NewAttribute("setup", "passive"), sdp.NewAttribute("connection", "new"))
	}
	body, err := description.Marshal()
	if err != nil {
		return nil, err
	}
	body = append(body, fmt.Sprintf("y=%010d\r\nf=v/2/5/25/1/4000a/1/8/1\r\n", parameters.ssrc)...)
	return body, nil
}

// validateGBAnswer 校验设备 200 OK 的 SDP 并返回设备实际使用的 SSRC。
// 与 wvp、Monibuca 一致：y= 缺失时沿用请求的 SSRC，y= 不同时以设备为准；
// f= 和 s= 只是描述信息，不参与校验。
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
			// GB 简捷 URI 可为数字设备 ID 加参数，并非 RFC URI；这里只校验媒体协商。
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
	// 省略方向时按 SDP 默认 sendrecv，只拒绝明确不发送媒体的应答。
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
