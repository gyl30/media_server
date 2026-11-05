package main

import (
	"context"
	"fmt"
	"io"
	"log/slog"
	"net"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"regexp"
	"sync"
	"testing"
	"time"

	"github.com/emiago/sipgo"
	"github.com/emiago/sipgo/sip"
)

func freeUDPAddress(t *testing.T) string {
	t.Helper()
	conn, err := net.ListenPacket("udp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	return conn.LocalAddr().String()
}

func TestBroadcastStopSucceedsWhenDeviceIgnoresBye(t *testing.T) {
	const platformID = "34020000002000000001"
	const deviceID = "34020000001320000001"
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))

	var mediaMu sync.Mutex
	var mediaPaths []string
	media := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		mediaMu.Lock()
		mediaPaths = append(mediaPaths, r.URL.Path)
		mediaMu.Unlock()
		if r.URL.Path == "/gb28181/audio/create" {
			w.Header().Set("Content-Type", "application/json")
			w.WriteHeader(http.StatusCreated)
			_, _ = io.WriteString(w, `{"rtp_port":40000}`)
			return
		}
		w.WriteHeader(http.StatusNoContent)
	}))
	defer media.Close()

	sources, err := openSourceStore(ctx, filepath.Join(t.TempDir(), "platform.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer sources.close()
	store, err := newDeviceStore(ctx, sources.db)
	if err != nil {
		t.Fatal(err)
	}
	if err := store.create(ctx, gbDevice{deviceID: deviceID, name: "camera"}); err != nil {
		t.Fatal(err)
	}

	platformAddress, deviceAddress := freeUDPAddress(t), freeUDPAddress(t)
	platformHost, platformPort, _ := net.SplitHostPort(platformAddress)
	_, devicePort, _ := net.SplitHostPort(deviceAddress)
	cfg := config{sipListen: platformAddress, sipAdvertise: platformAddress, sipID: platformID, sipDomain: "3402000000", sipPassword: "12345678",
		heartbeatTimeout: 90 * time.Second, mediaServer: mediaServer{controlURL: media.URL, mediaIP: "127.0.0.1"}}
	server, err := newSIPServer(cfg, store, logger)
	if err != nil {
		t.Fatal(err)
	}
	defer server.close()
	ssrcs, err := newSSRCAllocator(cfg.sipDomain)
	if err != nil {
		t.Fatal(err)
	}
	live := newLiveService(server, newMediaServerHTTPClient(cfg.mediaServer, 2*time.Second), ssrcs, 5*time.Second, 300*time.Millisecond, logger)
	go func() { _ = server.serve(ctx) }()

	var deviceContactPort int
	fmt.Sscan(devicePort, &deviceContactPort)
	var platformSIPPort int
	fmt.Sscan(platformPort, &platformSIPPort)
	server.devices.register(registeredDevice{id: deviceID, contact: sip.Uri{Scheme: "sip", User: deviceID, Host: "127.0.0.1", Port: deviceContactPort},
		remoteEndpoint: deviceAddress, expiresAt: time.Now().Add(time.Hour), lastHeartbeat: time.Now(), online: true})

	deviceUA, err := sipgo.NewUA(sipgo.WithUserAgent("camera"))
	if err != nil {
		t.Fatal(err)
	}
	defer deviceUA.Close()
	deviceServer, err := sipgo.NewServer(deviceUA)
	if err != nil {
		t.Fatal(err)
	}
	deviceClient, err := sipgo.NewClient(deviceUA, sipgo.WithClientConnectionAddr(deviceAddress), sipgo.WithClientHostname("127.0.0.1"))
	if err != nil {
		t.Fatal(err)
	}
	byeReceived := make(chan struct{}, 4)
	deviceServer.OnBye(func(req *sip.Request, tx sip.ServerTransaction) { byeReceived <- struct{}{} })
	deviceErrors := make(chan error, 1)
	notify := regexp.MustCompile(`<SN>(\d+)</SN><SourceID>(\d+)</SourceID>`)
	deviceServer.OnMessage(func(req *sip.Request, tx sip.ServerTransaction) {
		_ = tx.Respond(sip.NewResponseFromRequest(req, sip.StatusOK, "OK", nil))
		match := notify.FindSubmatch(req.Body())
		if match == nil {
			return
		}
		sn, sourceID := string(match[1]), string(match[2])
		go func() {
			deviceErrors <- answerBroadcast(ctx, deviceClient, platformID, deviceID, sourceID, sn, platformHost, platformSIPPort, deviceContactPort)
		}()
	})
	go func() { _ = deviceServer.ListenAndServe(ctx, "udp", deviceAddress) }()
	time.Sleep(200 * time.Millisecond)

	result, err := live.startBroadcast(ctx, deviceID)
	if err != nil {
		t.Fatalf("start broadcast: %v", err)
	}
	if err := <-deviceErrors; err != nil {
		t.Fatalf("device side: %v", err)
	}
	if err := live.stopBroadcast(ctx, deviceID, result.mediaID); err != nil {
		t.Fatalf("stop broadcast after unanswered BYE: %v", err)
	}
	select {
	case <-byeReceived:
	default:
		t.Fatal("platform did not send BYE")
	}
	live.mu.Lock()
	remaining := len(live.broadcasts)
	live.mu.Unlock()
	mediaMu.Lock()
	defer mediaMu.Unlock()
	if remaining != 0 || mediaPaths[len(mediaPaths)-1] != "/gb28181/audio/delete" {
		t.Fatalf("broadcasts %d, media requests %v", remaining, mediaPaths)
	}
}

func answerBroadcast(ctx context.Context, client *sipgo.Client, platformID, deviceID, sourceID, sn, platformHost string, platformPort, devicePort int) error {
	deviceURI := sip.Uri{Scheme: "sip", User: deviceID, Host: "127.0.0.1", Port: devicePort}
	fromParams := sip.NewParams()
	fromParams.Add("tag", sip.GenerateTagN(16))
	response := sip.NewRequest(sip.MESSAGE, sip.Uri{Scheme: "sip", User: platformID, Host: platformHost, Port: platformPort})
	response.AppendHeader(&sip.FromHeader{Address: deviceURI, Params: fromParams})
	response.AppendHeader(&sip.ToHeader{Address: sip.Uri{Scheme: "sip", User: platformID, Host: platformHost, Port: platformPort}})
	response.AppendHeader(sip.NewHeader("Content-Type", "Application/MANSCDP+xml"))
	response.SetBody([]byte(fmt.Sprintf("<?xml version=\"1.0\"?>\r\n<Response><CmdType>Broadcast</CmdType><SN>%s</SN><DeviceID>%s</DeviceID><Result>OK</Result></Response>\r\n", sn, deviceID)))
	reply, err := client.Do(ctx, response)
	if err != nil {
		return err
	}
	if !reply.IsSuccess() {
		return fmt.Errorf("broadcast response rejected with %d", reply.StatusCode)
	}
	target := sip.Uri{Scheme: "sip", User: sourceID, Host: platformHost, Port: platformPort}
	inviteFromParams := sip.NewParams()
	inviteFromParams.Add("tag", sip.GenerateTagN(16))
	offer := "v=0\r\no=" + deviceID + " 0 0 IN IP4 127.0.0.1\r\ns=Play\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\nm=audio 30000 RTP/AVP 8\r\na=recvonly\r\na=rtpmap:8 PCMA/8000\r\ny=0100000001\r\n"
	ua := sipgo.DialogUA{Client: client, ContactHDR: sip.ContactHeader{Address: deviceURI}}
	dialog, err := ua.Invite(ctx, target, []byte(offer), &sip.FromHeader{Address: deviceURI, Params: inviteFromParams}, &sip.ToHeader{Address: target},
		sip.NewHeader("Subject", sourceID+":0,"+deviceID+":0"), sip.NewHeader("Content-Type", "application/sdp"))
	if err != nil {
		return err
	}
	if err := dialog.WaitAnswer(ctx, sipgo.AnswerOptions{}); err != nil {
		return err
	}
	return dialog.Ack(ctx)
}
