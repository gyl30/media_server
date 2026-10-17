package main

import (
	"net/http"
	"strings"
	"testing"
)

func TestMediaHTTPOriginConfig(t *testing.T) {
	for _, origin := range []string{"", "http://127.0.0.1:18080", "https://video.example.test", "https://[::1]:8443/"} {
		t.Run(origin, func(t *testing.T) {
			cfg, err := parseConfig([]string{"--media-http-origin", origin})
			if err != nil {
				t.Fatal(err)
			}
			want := origin
			if len(want) > 0 && want[len(want)-1] == '/' {
				want = want[:len(want)-1]
			}
			if cfg.mediaServer.httpOrigin != want {
				t.Fatalf("origin=%q want=%q", cfg.mediaServer.httpOrigin, want)
			}
		})
	}
	for _, origin := range []string{"wss://host", "https://", "https://user:password@host", "https://host/audio", "https://host?", "https://host#", "https://host:0", "https://host:65536", "https://host:bad", "https://host:", "https://host/?a=b"} {
		if _, err := parseConfig([]string{"--media-http-origin", origin}); err == nil {
			t.Errorf("accepted invalid media HTTP origin %q", origin)
		}
	}
}

func TestMediaHTTPOriginURLs(t *testing.T) {
	for _, origin := range []string{"http://video.example.test:8088", "https://[2001:db8::1]:8443"} {
		t.Run(origin, func(t *testing.T) {
			s := testInfrastructure(t)
			s.media.server.httpOrigin = origin
			control, mediaIP := s.media.server.controlURL, s.media.server.mediaIP
			deviceID := createPushDevice(t, s)
			value := requestPushToken(t, s, deviceID)
			push := s.pushURLs(value)
			if push.WHIPURL != origin+"/publish/whip/"+value {
				t.Fatal("WHIP URL did not use the public origin")
			}
			verifyToken(t, s, value, "publish", value, http.StatusOK)
			testRegisteredMediaStream(s, value)
			play, err := s.newPlaybackURLs(playSource{PushDeviceID: deviceID})
			if err != nil {
				t.Fatal(err)
			}
			path := "/" + value + "/" + play.Token
			if play.FLVURL != origin+path+".flv" || play.HLSURL != origin+"/play/hls"+path+"/index.m3u8" || play.WHEPURL != origin+"/play/whep"+path {
				t.Fatal("HTTP playback URL path/token/origin changed")
			}
			for _, pair := range [][2]string{{push.RTMPURL, "rtmp://"}, {push.RTSPURL, "rtsp://"}, {play.RTMPURL, "rtmp://"}, {play.RTSPURL, "rtsp://"}} {
				if !strings.HasPrefix(pair[0], pair[1]+mediaIP+":") {
					t.Fatal("public HTTP origin changed RTMP/RTSP address")
				}
			}
			if s.media.server.controlURL != control || s.media.server.mediaIP != mediaIP {
				t.Fatal("public HTTP origin changed internal control or SIP media address")
			}
		})
	}
}
