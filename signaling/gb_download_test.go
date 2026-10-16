package main

import (
	"net/http"
	"net/http/httptest"
	"net/url"
	"path"
	"strings"
	"testing"
	"time"
)

func TestGBDownloadSDPBuild(t *testing.T) {
	parameters := liveSDPParameters{
		channelID: "34020000001320000002", mediaIP: "192.168.1.64", rtpPort: 30001, payloadType: 96, ssrc: 1200000001,
	}
	body, err := buildGBSDP(parameters, 1760000000, 1760000010, sessionDownload, 4)
	if err != nil {
		t.Fatal(err)
	}
	for _, line := range []string{"s=Download", "u=34020000001320000002:0", "t=1760000000 1760000010", "m=video 30001 TCP/RTP/AVP 96", "a=setup:passive", "a=connection:new", "a=downloadspeed:4", "y=1200000001"} {
		if !strings.Contains(string(body), line+"\r\n") {
			t.Errorf("missing %s in %s", line, body)
		}
	}
	for _, speed := range []int{0, 3, 8, -1} {
		if _, err := buildGBSDP(parameters, 1760000000, 1760000010, sessionDownload, speed); err == nil {
			t.Errorf("accepted download speed %d", speed)
		}
	}
	if playback, err := buildGBSDP(parameters, 1760000000, 1760000010, sessionPlayback, 1); err != nil || strings.Contains(string(playback), "downloadspeed") {
		t.Fatalf("playback SDP carries download speed: %s %v", playback, err)
	}
}

func TestGBDownloadBudgetRoundsUp(t *testing.T) {
	for _, test := range []struct {
		start, end int64
		speed      int
		want       int64
	}{{100, 110, 1, 10}, {100, 110, 4, 3}, {100, 108, 4, 2}, {100, 101, 2, 1}, {1, 1 << 62, 4, (1<<62 - 1 + 3) / 4}} {
		if got := historyMediaSeconds(test.start, test.end, test.speed); got != test.want {
			t.Errorf("seconds(%d,%d,%d)=%d want %d", test.start, test.end, test.speed, got, test.want)
		}
	}
}

// 真实 UDP SIP 对端：下载与回放共用 owner，认领前不 INVITE，INVITE 体为 Download 与倍速。
func TestGBDownloadCreateAndClaim(t *testing.T) {
	s := testInfrastructure(t)
	media := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/gb28181/receiver/playback/create":
			writeJSON(w, http.StatusCreated, map[string]int{"rtp_port": 30001})
		case "/receivers/delete":
			w.WriteHeader(http.StatusNoContent)
		default:
			t.Errorf("unexpected media request %s", r.URL.Path)
			w.WriteHeader(http.StatusNotFound)
		}
	}))
	t.Cleanup(media.Close)
	s.media.server.controlURL = media.URL
	invites, bodies := gbPlaybackPeer(t, s)
	downloadPath := "/api/devices/" + gbPlaybackDevice + "/channels/" + gbPlaybackChannel + "/download"
	for _, body := range []string{`{"start_time":1760000000,"end_time":1760000010}`, `{"start_time":1760000000,"end_time":1760000010,"speed":3}`,
		`{"start_time":1760000000,"end_time":1760000010,"speed":null}`, `{"start_time":1760000000,"end_time":1760000010,"speed":"4"}`,
		`{"start_time":1760000010,"end_time":1760000000,"speed":4}`} {
		apiRequest(t, s.handler(), "POST", downloadPath, body, http.StatusBadRequest)
	}
	// 回放只要出现 speed 字段即拒绝，包括 0 与 null。
	for _, speed := range []string{"4", "1", "0", "null"} {
		apiRequest(t, s.handler(), "POST", gbPlaybackPath, `{"start_time":1760000000,"end_time":1760000010,"speed":`+speed+`}`, http.StatusBadRequest)
	}
	before := time.Now()
	ticket, token := gbPlaybackTicket(t, apiRequest(t, s.handler(), "POST", downloadPath, `{"start_time":1760000000,"end_time":1760000010,"speed":4}`, http.StatusCreated))
	// 预算为 ceil(10/4)=3 秒 + 票据 TTL + 60 秒宽限，从创建时开始计算。
	budget := ticket.Deadline.Sub(before)
	want := 3*time.Second + s.tokenTTL + playbackCompletionGrace
	if budget < want-time.Second || budget > want+time.Second {
		t.Fatalf("download budget=%s want≈%s", budget, want)
	}
	gbPlaybackNoInvite(t, bodies)
	parsed, err := url.Parse(ticket.PSURL)
	if err != nil {
		t.Fatal(err)
	}
	if strings.TrimSuffix(path.Base(parsed.Path), ".ps") != token {
		t.Fatal("ticket token mismatch")
	}
	verifyToken(t, s, token, "play", ticket.PlaybackID, http.StatusOK)
	var offer []byte
	select {
	case offer = <-bodies:
	case <-time.After(3 * time.Second):
		t.Fatal("download INVITE missing")
	}
	for _, line := range []string{"s=Download", "a=downloadspeed:4", "t=1760000000 1760000010", "m=video 30001 TCP/RTP/AVP 96"} {
		if !strings.Contains(string(offer), line+"\r\n") {
			t.Errorf("download offer missing %s: %s", line, offer)
		}
	}
	session := gbPlaybackFind(t, s, ticket.PlaybackID)
	gbPlaybackWait(t, session.established, "下载建立")
	if invites.Load() != 1 {
		t.Fatalf("INVITE=%d", invites.Load())
	}
	apiRequest(t, s.handler(), "DELETE", "/api/playbacks/"+ticket.PlaybackID, "", http.StatusNoContent)
	gbPlaybackResources(t, s, 0, 0, 0)
}
