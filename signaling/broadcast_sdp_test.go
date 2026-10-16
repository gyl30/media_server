package main

import (
	"strings"
	"testing"
)

func TestBroadcastOffer(t *testing.T) {
	base := "v=0\r\no=peer 0 0 IN IP4 127.0.0.1\r\ns=Play\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\nm=audio 20000 RTP/AVP 8 96\r\na=recvonly\r\na=rtpmap:8 PCMA/8000/1\r\ny=0123456789\r\n"
	for _, test := range []struct {
		name, body string
		valid      bool
		rtcp       uint16
	}{
		{name: "observed profile", body: base, valid: true, rtcp: 20001},
		{name: "static PT and no y", body: strings.ReplaceAll(strings.ReplaceAll(base, "a=rtpmap:8 PCMA/8000/1\r\n", ""), "y=0123456789\r\n", ""), valid: true, rtcp: 20001},
		{name: "explicit RTCP", body: base + "a=rtcp:30001 IN IP4 127.0.0.1\r\n", valid: true, rtcp: 30001},
		{name: "unregistered media IP", body: strings.ReplaceAll(base, "c=IN IP4 127.0.0.1", "c=IN IP4 192.0.2.1")},
		{name: "invalid PCMA map", body: strings.ReplaceAll(base, "PCMA/8000/1", "PCMU/8000/1")},
		{name: "stereo", body: strings.ReplaceAll(base, "PCMA/8000/1", "PCMA/8000/2")},
		{name: "wrong direction", body: strings.ReplaceAll(base, "a=recvonly", "a=sendonly")},
		{name: "missing direction", body: strings.ReplaceAll(base, "a=recvonly\r\n", "")},
		{name: "missing PCMA", body: strings.ReplaceAll(base, "RTP/AVP 8 96", "RTP/AVP 96")},
		{name: "TCP", body: strings.ReplaceAll(base, "RTP/AVP", "TCP/RTP/AVP")},
		{name: "duplicate y", body: base + "y=0123456789\r\n"},
		{name: "zero y", body: strings.ReplaceAll(base, "0123456789", "0")},
		{name: "overflow y", body: strings.ReplaceAll(base, "0123456789", "4294967296")},
		{name: "unregistered RTCP IP", body: base + "a=rtcp:30001 IN IP4 192.0.2.1\r\n"},
		{name: "malformed RTCP", body: base + "a=rtcp:\r\n"},
		{name: "same RTP RTCP", body: base + "a=rtcp:20000\r\n"},
		{name: "port pair overflow", body: strings.ReplaceAll(base, "audio 20000", "audio 65535")},
		{name: "multiple media", body: base + "m=video 20002 RTP/AVP 96\r\n"},
	} {
		t.Run(test.name, func(t *testing.T) {
			peer, err := parseBroadcastOffer([]byte(test.body), "127.0.0.1:5060")
			if (err == nil) != test.valid {
				t.Fatalf("valid=%v error=%v", test.valid, err)
			}
			if test.valid && (peer.ssrc == 0 || peer.endpoint.rtcp.Port() != test.rtcp || peer.endpoint.rtp.Port() != 20000) {
				t.Fatalf("unexpected negotiated peer: %+v", peer)
			}
		})
	}
}
