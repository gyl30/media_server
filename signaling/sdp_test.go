package main

import (
	"strings"
	"testing"
)

func TestBuildAndValidateLiveUDPSDP(t *testing.T) {
	parameters := liveSDPParameters{
		channelID:   testChannelID,
		mediaIP:     "192.0.2.20",
		rtpPort:     40000,
		payloadType: 96,
		ssrc:        200000001,
	}
	body, err := buildLiveUDPSDP(parameters)
	if err != nil {
		t.Fatalf("buildLiveUDPSDP() error = %v", err)
	}
	text := string(body)
	for _, field := range []string{
		"v=0\r\n",
		"o=" + testChannelID + " 0 0 IN IP4 192.0.2.20\r\n",
		"s=Play\r\n",
		"c=IN IP4 192.0.2.20\r\n",
		"t=0 0\r\n",
		"m=video 40000 RTP/AVP 96\r\n",
		"a=recvonly\r\n",
		"a=rtpmap:96 PS/90000\r\n",
		"y=0200000001\r\n",
		"f=v/2/5/25/1/4000a/1/8/1\r\n",
	} {
		if !strings.Contains(text, field) {
			t.Fatalf("SDP missing %q:\n%s", field, text)
		}
	}

	answer := []byte(strings.ReplaceAll(text, "a=recvonly", "a=sendonly"))
	if err := validateLiveUDPAnswer(answer, parameters.payloadType, parameters.ssrc); err != nil {
		t.Fatalf("validateLiveUDPAnswer() error = %v", err)
	}
}

func TestValidateLiveUDPAnswerRejectsProtocolMismatch(t *testing.T) {
	valid := "v=0\r\n" +
		"o=" + testChannelID + " 0 0 IN IP4 192.0.2.30\r\n" +
		"s=Play\r\n" +
		"c=IN IP4 192.0.2.30\r\n" +
		"t=0 0\r\n" +
		"m=video 6000 RTP/AVP 96\r\n" +
		"a=sendonly\r\n" +
		"a=rtpmap:96 PS/90000\r\n" +
		"y=0200000001\r\n" +
		"f=v/2/5/25/1/4000a/1/8/1\r\n"
	for _, body := range []string{
		strings.Replace(valid, "RTP/AVP", "TCP/RTP/AVP", 1),
		strings.Replace(valid, "PS/90000", "H264/90000", 1),
		strings.Replace(valid, "y=0200000001", "y=0200000002", 1),
		strings.Replace(valid, "a=sendonly", "a=recvonly", 1),
		strings.Replace(valid, "a=sendonly\r\n", "", 1),
		strings.Replace(valid, "a=sendonly", "a=sendrecv", 1),
		strings.Replace(valid, "a=sendonly\r\n", "a=sendonly\r\ny=\r\n", 1),
		strings.Replace(valid, "f=v/2/5/25/1/4000a/1/8/1\r\n", "", 1),
	} {
		if err := validateLiveUDPAnswer([]byte(body), 96, 200000001); err == nil {
			t.Fatalf("validateLiveUDPAnswer() accepted:\n%s", body)
		}
	}
}

func TestValidateLiveUDPAnswerRejectsSessionLevelReceiveOnly(t *testing.T) {
	body := "v=0\r\n" +
		"o=" + testChannelID + " 0 0 IN IP4 192.0.2.30\r\n" +
		"s=Play\r\n" +
		"c=IN IP4 192.0.2.30\r\n" +
		"t=0 0\r\n" +
		"a=recvonly\r\n" +
		"m=video 6000 RTP/AVP 96\r\n" +
		"a=rtpmap:96 PS/90000\r\n" +
		"y=0200000001\r\n" +
		"f=v/2/5/25/1/4000a/1/8/1\r\n"
	if err := validateLiveUDPAnswer([]byte(body), 96, 200000001); err == nil {
		t.Fatal("validateLiveUDPAnswer() accepted session recvonly")
	}
}
