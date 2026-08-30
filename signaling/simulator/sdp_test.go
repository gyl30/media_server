package main

import (
	"strings"
	"testing"
)

func TestParseLiveOffer(t *testing.T) {
	body := []byte("v=0\r\no=34020000001320000002 0 0 IN IP4 127.0.0.1\r\ns=Play\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\nm=video 40000 RTP/AVP 96\r\na=recvonly\r\na=rtpmap:96 PS/90000\r\ny=0200000001\r\nf=v/2/5/25/1/4000a/1/8/1\r\n")
	target, answer, err := parseLiveOffer(body)
	if err != nil {
		t.Fatalf("parseLiveOffer() error = %v", err)
	}
	if target.address != "127.0.0.1" || target.rtpPort != 40000 || target.payloadType != 96 || target.ssrc != 200000001 {
		t.Fatalf("target = %+v", target)
	}
	if !strings.Contains(string(answer), "a=sendonly") || strings.Contains(string(answer), "a=recvonly") {
		t.Fatalf("answer = %q", answer)
	}
}

func TestParseLiveOfferRejectsInvalidMedia(t *testing.T) {
	for name, body := range map[string]string{
		"wrong codec":     "v=0\r\no=x 0 0 IN IP4 127.0.0.1\r\ns=Play\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\nm=video 40000 RTP/AVP 96\r\na=recvonly\r\na=rtpmap:96 H264/90000\r\ny=0200000001\r\n",
		"wrong direction": "v=0\r\no=x 0 0 IN IP4 127.0.0.1\r\ns=Play\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\nm=video 40000 RTP/AVP 96\r\na=sendonly\r\na=rtpmap:96 PS/90000\r\ny=0200000001\r\n",
		"missing ssrc":    "v=0\r\no=x 0 0 IN IP4 127.0.0.1\r\ns=Play\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\nm=video 40000 RTP/AVP 96\r\na=recvonly\r\na=rtpmap:96 PS/90000\r\n",
	} {
		t.Run(name, func(t *testing.T) {
			if _, _, err := parseLiveOffer([]byte(body)); err == nil {
				t.Fatal("parseLiveOffer() succeeded")
			}
		})
	}
}
