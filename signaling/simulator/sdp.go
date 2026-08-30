package main

import (
	"fmt"
	"net"
	"strconv"
	"strings"

	"github.com/pion/sdp/v3"
)

type mediaTarget struct {
	address     string
	rtpPort     uint16
	payloadType uint8
	ssrc        uint32
}

func parseLiveOffer(body []byte) (mediaTarget, []byte, error) {
	var standard strings.Builder
	var y string
	for _, line := range strings.Split(strings.ReplaceAll(string(body), "\r\n", "\n"), "\n") {
		switch {
		case strings.HasPrefix(line, "y="):
			if y != "" {
				return mediaTarget{}, nil, fmt.Errorf("duplicate SDP y field")
			}
			y = strings.TrimPrefix(line, "y=")
		case strings.HasPrefix(line, "f="):
			// GB28181 extension field; pion/sdp does not parse it.
		case line != "":
			standard.WriteString(line)
			standard.WriteString("\r\n")
		}
	}
	ssrc64, err := strconv.ParseUint(y, 10, 32)
	if err != nil || ssrc64 == 0 {
		return mediaTarget{}, nil, fmt.Errorf("invalid SDP SSRC")
	}
	var description sdp.SessionDescription
	if err := description.UnmarshalString(standard.String()); err != nil {
		return mediaTarget{}, nil, err
	}
	if description.ConnectionInformation == nil || description.ConnectionInformation.Address == nil || len(description.MediaDescriptions) != 1 {
		return mediaTarget{}, nil, fmt.Errorf("invalid live SDP")
	}
	address := description.ConnectionInformation.Address.Address
	if ip := net.ParseIP(address); ip == nil || ip.IsUnspecified() {
		return mediaTarget{}, nil, fmt.Errorf("invalid media address")
	}
	media := description.MediaDescriptions[0]
	if media.MediaName.Media != "video" || media.MediaName.Port.Value <= 0 || media.MediaName.Port.Value > 65535 || strings.Join(media.MediaName.Protos, "/") != "RTP/AVP" || len(media.MediaName.Formats) != 1 {
		return mediaTarget{}, nil, fmt.Errorf("invalid media description")
	}
	payload64, err := strconv.ParseUint(media.MediaName.Formats[0], 10, 8)
	if err != nil || payload64 > 127 {
		return mediaTarget{}, nil, fmt.Errorf("invalid payload type")
	}
	rtpmap, ok := media.Attribute("rtpmap")
	if !ok || !strings.EqualFold(rtpmap, fmt.Sprintf("%d PS/90000", payload64)) {
		return mediaTarget{}, nil, fmt.Errorf("invalid payload mapping")
	}
	if direction(media) != "recvonly" {
		return mediaTarget{}, nil, fmt.Errorf("platform SDP is not recvonly")
	}
	answer := strings.Replace(string(body), "a=recvonly", "a=sendonly", 1)
	if answer == string(body) {
		return mediaTarget{}, nil, fmt.Errorf("missing recvonly attribute")
	}
	return mediaTarget{
		address:     address,
		rtpPort:     uint16(media.MediaName.Port.Value),
		payloadType: uint8(payload64),
		ssrc:        uint32(ssrc64),
	}, []byte(answer), nil
}

func direction(media *sdp.MediaDescription) string {
	for _, attribute := range media.Attributes {
		switch attribute.Key {
		case "sendonly", "recvonly", "sendrecv", "inactive":
			return attribute.Key
		}
	}
	return ""
}
