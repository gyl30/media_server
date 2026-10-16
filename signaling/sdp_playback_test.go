package main

import (
	"strings"
	"testing"
)

func TestGBSDPBuildLiveContractUnchanged(t *testing.T) {
	body, err := buildGBSDP(liveSDPParameters{
		channelID: "34020000001320000002", mediaIP: "192.168.1.64", rtpPort: 30000, payloadType: 96, ssrc: 200000001,
	}, 0, 0, sessionPlay, 0)
	want := "v=0\r\no=34020000001320000002 0 0 IN IP4 192.168.1.64\r\ns=Play\r\nc=IN IP4 192.168.1.64\r\nt=0 0\r\nm=video 30000 RTP/AVP 96\r\na=recvonly\r\na=rtpmap:96 PS/90000\r\ny=0200000001\r\nf=v/2/5/25/1/4000a/1/8/1\r\n"
	if err != nil || string(body) != want {
		t.Fatalf("live SDP changed: %q err=%v", body, err)
	}
}

func TestGBPlaybackSDPBuild(t *testing.T) {
	parameters := liveSDPParameters{
		channelID: "34020000001320000002", mediaIP: "192.168.1.64", rtpPort: 30001, payloadType: 96, ssrc: 1200000001,
	}
	body, err := buildGBSDP(parameters, 1760000000, 1760000010, sessionPlayback, 1)
	if err != nil {
		t.Fatal(err)
	}
	for _, line := range []string{"s=Playback", "u=34020000001320000002:0", "t=1760000000 1760000010", "m=video 30001 TCP/RTP/AVP 96", "a=setup:passive", "a=connection:new", "a=recvonly", "y=1200000001"} {
		if !strings.Contains(string(body), line+"\r\n") {
			t.Errorf("missing %s in %s", line, body)
		}
	}
	answer := strings.ReplaceAll(strings.ReplaceAll(string(body), "a=recvonly", "a=sendonly"), "a=setup:passive", "a=setup:active")
	if ssrc, err := validateGBAnswer([]byte(answer), 96, parameters.ssrc, true); err != nil || ssrc != parameters.ssrc {
		t.Fatalf("GB numeric shorthand u rejected: ssrc=%d err=%v", ssrc, err)
	}
	for _, times := range [][2]int64{{-1, 10}, {10, 10}, {11, 10}} {
		if _, err := buildGBSDP(parameters, times[0], times[1], sessionPlayback, 1); err == nil {
			t.Errorf("accepted invalid time range %v", times)
		}
	}
	parameters.mediaIP = "2001:db8::10"
	if body, err := buildGBSDP(parameters, 0, 10, sessionPlayback, 1); err != nil || !strings.Contains(string(body), "IN IP6 2001:db8::10") {
		t.Fatalf("IPv6 build: %s %v", body, err)
	}
}

func TestGBPlaybackAnswerNegotiation(t *testing.T) {
	base := "v=0\r\no=34020000001320000002 0 0 IN IP4 192.168.1.64\r\ns=Playback\r\nu=34020000001320000002:0\r\nc=IN IP4 192.168.1.64\r\nt=1760000000 1760000010\r\nm=video 30001 TCP/RTP/AVP 96\r\na=sendonly\r\na=rtpmap:96 PS/90000\r\na=setup:active\r\na=connection:new\r\ny=1200000001\r\n"
	for _, test := range []struct {
		name, old, replacement string
		want                   uint32
	}{
		{"standard", "", "", 1200000001},
		{"missing_y", "y=1200000001\r\n", "", 1200000001},
		{"device_y", "y=1200000001", "y=1200000019", 1200000019},
		{"default_direction", "a=sendonly\r\n", "", 1200000001},
		{"sendrecv", "a=sendonly", "a=sendrecv", 1200000001},
		{"udp", "TCP/RTP/AVP", "RTP/AVP", 0},
		{"wrong_payload", "m=video 30001 TCP/RTP/AVP 96", "m=video 30001 TCP/RTP/AVP 98", 0},
		{"zero_port", "m=video 30001", "m=video 0", 0},
		{"recvonly", "a=sendonly", "a=recvonly", 0},
		{"inactive", "a=sendonly", "a=inactive", 0},
		{"passive", "a=setup:active", "a=setup:passive", 0},
		{"actpass", "a=setup:active", "a=setup:actpass", 0},
		{"missing_setup", "a=setup:active\r\n", "", 0},
		{"reuse_connection", "a=connection:new", "a=connection:existing", 0},
		{"missing_connection", "a=connection:new\r\n", "", 0},
		{"invalid_y", "y=1200000001", "y=no", 0},
		{"duplicate_y", "y=1200000001", "y=1200000001\r\ny=1200000001", 0},
	} {
		t.Run(test.name, func(t *testing.T) {
			body := base
			if test.old != "" {
				body = strings.Replace(body, test.old, test.replacement, 1)
			}
			ssrc, err := validateGBAnswer([]byte(body), 96, 1200000001, true)
			if (err == nil) != (test.want != 0) || ssrc != test.want {
				t.Fatalf("ssrc=%d want=%d err=%v", ssrc, test.want, err)
			}
		})
	}
	// TCP negotiation attributes may be inherited from the session level.
	sessionAttributes := strings.ReplaceAll(base, "a=setup:active\r\na=connection:new\r\n", "")
	sessionAttributes = strings.Replace(sessionAttributes, "m=video", "a=setup:active\r\na=connection:new\r\nm=video", 1)
	if _, err := validateGBAnswer([]byte(sessionAttributes), 96, 1200000001, true); err != nil {
		t.Fatalf("session-level TCP attributes rejected: %v", err)
	}
	for _, attribute := range []string{"setup", "connection"} {
		body := strings.Replace(sessionAttributes, "a=sendonly", "a=sendonly\r\na="+attribute+":", 1)
		if _, err := validateGBAnswer([]byte(body), 96, 1200000001, true); err == nil {
			t.Errorf("empty media-level %s must not inherit valid session attribute", attribute)
		}
	}
	if _, err := validateGBAnswer([]byte(base), 96, 1200000001, false); err == nil {
		t.Fatal("live UDP accepted TCP answer")
	}
}
