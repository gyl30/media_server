package main

import (
	"fmt"
	"math/rand/v2"
	"net/netip"
	"strconv"
	"strings"

	"github.com/pion/sdp/v3"
)

// 首个控制子集仅接受注册来源同 IP 的单路 UDP PCMA，媒体所有权由调用方保留。
func parseBroadcastOffer(body []byte, registeredSource string) (broadcastPeer, error) {
	remote, err := netip.ParseAddrPort(registeredSource)
	if err != nil || !remote.Addr().Is4() {
		return broadcastPeer{}, fmt.Errorf("invalid broadcast source")
	}
	var standard strings.Builder
	var ssrc uint32
	seenY := false
	for line := range strings.SplitSeq(strings.ReplaceAll(string(body), "\r\n", "\n"), "\n") {
		line = strings.TrimSpace(line)
		switch {
		case strings.HasPrefix(line, "y="):
			value, err := strconv.ParseUint(strings.TrimPrefix(line, "y="), 10, 32)
			if seenY || err != nil || value == 0 {
				return broadcastPeer{}, fmt.Errorf("invalid broadcast SSRC")
			}
			seenY, ssrc = true, uint32(value)
		case strings.HasPrefix(line, "f="):
		case line != "":
			standard.WriteString(line)
			standard.WriteString("\r\n")
		}
	}
	var description sdp.SessionDescription
	if err := description.UnmarshalString(standard.String()); err != nil {
		return broadcastPeer{}, err
	}
	if len(description.MediaDescriptions) != 1 {
		return broadcastPeer{}, fmt.Errorf("expected one broadcast audio section")
	}
	media := description.MediaDescriptions[0]
	port := media.MediaName.Port
	if media.MediaName.Media != "audio" || strings.Join(media.MediaName.Protos, "/") != "RTP/AVP" ||
		port.Value <= 0 || port.Value >= 65535 || port.Range != nil {
		return broadcastPeer{}, fmt.Errorf("invalid broadcast media transport")
	}
	pcma := false
	for _, format := range media.MediaName.Formats {
		pcma = pcma || format == "8"
	}
	connection := media.ConnectionInformation
	if connection == nil {
		connection = description.ConnectionInformation
	}
	if !pcma || connection == nil || connection.NetworkType != "IN" || connection.AddressType != "IP4" || connection.Address == nil ||
		connection.Address.Address != remote.Addr().String() || connection.Address.TTL != nil || connection.Address.Range != nil {
		return broadcastPeer{}, fmt.Errorf("broadcast address or PCMA mismatch")
	}
	direction := "sendrecv"
	for _, attributes := range [][]sdp.Attribute{description.Attributes, media.Attributes} {
		for _, attribute := range attributes {
			switch attribute.Key {
			case "sendonly", "recvonly", "sendrecv", "inactive":
				direction = attribute.Key
			case "rtpmap":
				fields := strings.Fields(attribute.Value)
				if len(fields) != 2 || (fields[0] == "8" && !strings.EqualFold(fields[1], "PCMA/8000") && !strings.EqualFold(fields[1], "PCMA/8000/1")) {
					return broadcastPeer{}, fmt.Errorf("invalid PCMA payload mapping")
				}
			}
		}
	}
	if direction != "recvonly" {
		return broadcastPeer{}, fmt.Errorf("broadcast receiver must be recvonly")
	}
	rtcpPort := port.Value + 1
	if value, explicit := media.Attribute("rtcp"); explicit {
		fields := strings.Fields(value)
		if len(fields) != 1 && (len(fields) != 4 || fields[1] != "IN" || fields[2] != "IP4" || fields[3] != remote.Addr().String()) {
			return broadcastPeer{}, fmt.Errorf("invalid broadcast RTCP address")
		}
		rtcpPort, err = strconv.Atoi(fields[0])
		if err != nil || rtcpPort <= 0 || rtcpPort > 65535 || rtcpPort == port.Value {
			return broadcastPeer{}, fmt.Errorf("invalid broadcast RTCP port")
		}
	}
	if !seenY {
		for ssrc == 0 {
			ssrc = rand.Uint32()
		}
	}
	return broadcastPeer{
		endpoint: broadcastEndpoint{rtp: netip.AddrPortFrom(remote.Addr(), uint16(port.Value)), rtcp: netip.AddrPortFrom(remote.Addr(), uint16(rtcpPort))},
		ssrc:     ssrc,
	}, nil
}

func buildBroadcastAnswer(sourceID string, local broadcastEndpoint, ssrc uint32) []byte {
	return []byte(fmt.Sprintf("v=0\r\no=%s 0 0 IN IP4 %s\r\ns=Play\r\nc=IN IP4 %s\r\nt=0 0\r\nm=audio %d RTP/AVP 8\r\na=sendonly\r\na=rtpmap:8 PCMA/8000/1\r\na=rtcp:%d IN IP4 %s\r\ny=%010d\r\n",
		sourceID, local.rtp.Addr(), local.rtp.Addr(), local.rtp.Port(), local.rtcp.Port(), local.rtcp.Addr(), ssrc))
}
