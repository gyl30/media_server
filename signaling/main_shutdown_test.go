package main

import (
	"context"
	"encoding/json"
	"encoding/xml"
	"fmt"
	"io"
	"log/slog"
	"net"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/emiago/sipgo"
	"github.com/emiago/sipgo/sip"
)

func TestRunDrainsHTTPBeforeStoppingSIP(t *testing.T) {
	tcp, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	httpAddress := tcp.Addr().String()
	_ = tcp.Close()
	udp, err := net.ListenPacket("udp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	sipAddress := udp.LocalAddr().String()
	_ = udp.Close()
	media := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, http.StatusOK, map[string]any{"sources": []any{}})
	}))
	defer media.Close()
	ready := make(sipgo.ListenReadyCtxValue, 1)
	ctx, cancel := context.WithCancel(context.WithValue(t.Context(), sipgo.ListenReadyCtxKey, ready))
	done := make(chan error, 1)
	go func() {
		done <- run(ctx, []string{"--database", filepath.Join(t.TempDir(), "run.db"), "--sip-listen", sipAddress, "--sip-advertise", sipAddress, "--http-listen", httpAddress, "--media-control-url", media.URL}, slog.New(slog.NewTextHandler(io.Discard, nil)))
	}()
	t.Cleanup(func() {
		cancel()
		select {
		case err := <-done:
			if err != nil {
				t.Errorf("run: %v", err)
			}
		case <-time.After(8 * time.Second):
			t.Error("run did not stop")
		}
	})
	select {
	case <-ready:
	case <-time.After(2 * time.Second):
		t.Fatal("SIP not ready")
	}
	transport := &http.Transport{DisableKeepAlives: true}
	defer transport.CloseIdleConnections()
	client := &http.Client{Transport: transport, Timeout: 7 * time.Second}
	base := "http://" + httpAddress
	deadline := time.Now().Add(2 * time.Second)
	for {
		response, err := client.Get(base + "/api/devices")
		if err == nil {
			_ = response.Body.Close()
			if response.StatusCode == http.StatusOK {
				break
			}
		}
		if time.Now().After(deadline) {
			t.Fatal("HTTP not ready")
		}
		time.Sleep(5 * time.Millisecond)
	}
	response, err := client.Post(base+"/api/devices", "application/json", strings.NewReader(`{"device_id":"`+queryTestDeviceID+`","name":"run drain"}`))
	if err != nil {
		t.Fatal(err)
	}
	_ = response.Body.Close()
	if response.StatusCode != http.StatusCreated {
		t.Fatalf("create HTTP %d", response.StatusCode)
	}
	listener, err := net.ListenPacket("udp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	ua, err := sipgo.NewUA()
	if err != nil {
		_ = listener.Close()
		t.Fatal(err)
	}
	peerServer, err := sipgo.NewServer(ua)
	if err != nil {
		_ = listener.Close()
		_ = ua.Close()
		t.Fatal(err)
	}
	peerClient, err := sipgo.NewClient(ua, sipgo.WithClientAddr(listener.LocalAddr().String()), sipgo.WithClientConnectionAddr(listener.LocalAddr().String()))
	if err != nil {
		_ = listener.Close()
		_ = ua.Close()
		t.Fatal(err)
	}
	queryReceived, releaseXML := make(chan struct{}), make(chan struct{})
	catalogReady, ptzStarted, ptzStopped := make(chan struct{}), make(chan struct{}), make(chan struct{})
	var releaseOnce sync.Once
	defer releaseOnce.Do(func() { close(releaseXML) })
	peerServer.OnMessage(func(request *sip.Request, transaction sip.ServerTransaction) {
		var query manscdpEnvelope
		if err := xml.Unmarshal(request.Body(), &query); err != nil {
			t.Error(err)
			return
		}
		if err := transaction.Respond(sip.NewResponseFromRequest(request, sip.StatusOK, "OK", nil)); err != nil {
			t.Error(err)
			return
		}
		if query.CmdType == "DeviceControl" {
			var control ptzControl
			if err := xml.Unmarshal(request.Body(), &control); err != nil {
				t.Error(err)
				return
			}
			if control.PTZCommand == "A50F0000000000B4" {
				close(ptzStopped)
			} else {
				close(ptzStarted)
			}
			return
		}
		var body []byte
		if query.CmdType == "DeviceInfo" {
			close(queryReceived)
			<-releaseXML
			body = queryResponseXML(manscdpQuery{CmdType: query.CmdType, SN: query.SN, DeviceID: query.DeviceID}, "OK", "<DeviceName>drain-completed</DeviceName>")
		} else if query.CmdType == "Catalog" {
			body = []byte(fmt.Sprintf(`<Response><CmdType>Catalog</CmdType><SN>%d</SN><DeviceID>%s</DeviceID><SumNum>1</SumNum><DeviceList Num="1"><Item><DeviceID>%s</DeviceID><Name>drain channel</Name><Status>ON</Status></Item></DeviceList></Response>`, query.SN, queryTestDeviceID, presetQueryChannelID))
		} else {
			return
		}
		message := sip.NewRequest(sip.MESSAGE, sip.Uri{Scheme: "sip", User: "34020000002000000001", Host: "127.0.0.1"})
		message.AppendHeader(&sip.FromHeader{Address: sip.Uri{Scheme: "sip", User: queryTestDeviceID, Host: "127.0.0.1"}, Params: sip.NewParams()})
		message.AppendHeader(&sip.ToHeader{Address: message.Recipient})
		message.AppendHeader(sip.NewHeader("Content-Type", "Application/MANSCDP+xml"))
		message.SetBody(body)
		message.SetTransport("UDP")
		message.SetDestination(sipAddress)
		messageCtx, stop := context.WithTimeout(context.Background(), time.Second)
		defer stop()
		answer, err := peerClient.Do(messageCtx, message)
		if err != nil {
			t.Errorf("drain XML: %v", err)
		} else if answer.StatusCode != sip.StatusOK {
			t.Errorf("drain XML SIP %d", answer.StatusCode)
		}
		if query.CmdType == "Catalog" && err == nil && answer.StatusCode == sip.StatusOK {
			close(catalogReady)
		}
	})
	peerDone := make(chan error, 1)
	peerServer.OnOptions(func(request *sip.Request, transaction sip.ServerTransaction) {
		_ = transaction.Respond(sip.NewResponseFromRequest(request, sip.StatusOK, "OK", nil))
	})
	go func() { peerDone <- peerServer.ServeUDP(listener) }()
	t.Cleanup(func() { releaseOnce.Do(func() { close(releaseXML) }); _ = listener.Close(); <-peerDone; _ = ua.Close() })
	// 独立 OPTIONS 完成证明 peer 的 UDP listener 已进入 sipgo 连接池，再复用其端口发 REGISTER。
	probe, err := net.Dial("udp", listener.LocalAddr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer probe.Close()
	_ = probe.SetDeadline(time.Now().Add(time.Second))
	if _, err := fmt.Fprintf(probe, "OPTIONS sip:ready@127.0.0.1 SIP/2.0\r\nVia: SIP/2.0/UDP %s;branch=z9hG4bK-ready\r\nFrom: <sip:ready@127.0.0.1>;tag=ready\r\nTo: <sip:ready@127.0.0.1>\r\nCall-ID: ready\r\nCSeq: 1 OPTIONS\r\nContent-Length: 0\r\n\r\n", probe.LocalAddr()); err != nil {
		t.Fatal(err)
	}
	probeResult := make([]byte, 4096)
	probeLength, err := probe.Read(probeResult)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.HasPrefix(string(probeResult[:probeLength]), "SIP/2.0 200 ") {
		t.Fatalf("peer OPTIONS: %s", probeResult[:probeLength])
	}
	host, portText, _ := net.SplitHostPort(listener.LocalAddr().String())
	port, _ := strconv.Atoi(portText)
	identity := sip.Uri{Scheme: "sip", User: queryTestDeviceID, Host: "3402000000"}
	registration := sip.NewRequest(sip.REGISTER, sip.Uri{Scheme: "sip", Host: "127.0.0.1"})
	registration.AppendHeader(&sip.FromHeader{Address: identity, Params: sip.NewParams()})
	registration.AppendHeader(&sip.ToHeader{Address: identity})
	registration.AppendHeader(&sip.ContactHeader{Address: sip.Uri{Scheme: "sip", User: queryTestDeviceID, Host: host, Port: port}})
	expires := sip.ExpiresHeader(120)
	registration.AppendHeader(&expires)
	registration.SetTransport("UDP")
	registration.SetDestination(sipAddress)
	registerCtx, stopRegister := context.WithTimeout(t.Context(), time.Second)
	defer stopRegister()
	challenge, err := peerClient.Do(registerCtx, registration)
	if err != nil {
		t.Fatal(err)
	}
	if challenge.StatusCode != sip.StatusUnauthorized {
		t.Fatalf("REGISTER challenge %d", challenge.StatusCode)
	}
	accepted, err := peerClient.DoDigestAuth(registerCtx, registration, challenge, sipgo.DigestAuth{Username: queryTestDeviceID, Password: "12345678"})
	if err != nil {
		t.Fatal(err)
	}
	if accepted.StatusCode != sip.StatusOK {
		t.Fatalf("REGISTER SIP %d", accepted.StatusCode)
	}
	select {
	case <-catalogReady:
	case <-time.After(time.Second):
		t.Fatal("Catalog not ready")
	}
	queryDone := make(chan error, 1)
	go func() {
		response, err := client.Post(base+"/api/devices/"+queryTestDeviceID+"/query", "application/json", strings.NewReader(`{"command":"DeviceInfo"}`))
		if err != nil {
			queryDone <- err
			return
		}
		defer response.Body.Close()
		var result deviceQueryResponse
		err = json.NewDecoder(response.Body).Decode(&result)
		if err == nil && (response.StatusCode != http.StatusOK || result.Info == nil || result.Info.DeviceName != "drain-completed") {
			err = fmt.Errorf("query HTTP %d response %+v", response.StatusCode, result)
		}
		queryDone <- err
	}()
	select {
	case <-queryReceived:
	case <-time.After(2 * time.Second):
		t.Fatal("query not received")
	}
	ptzDone := make(chan error, 1)
	go func() {
		response, err := client.Post(base+"/api/devices/"+queryTestDeviceID+"/channels/"+presetQueryChannelID+"/ptz", "application/json", strings.NewReader(`{"command":"left","speed":64}`))
		if err != nil {
			ptzDone <- err
			return
		}
		defer response.Body.Close()
		body, err := io.ReadAll(response.Body)
		if err == nil && response.StatusCode != http.StatusOK && !(response.StatusCode == http.StatusBadGateway && strings.Contains(string(body), "ptz_start_failed")) {
			err = fmt.Errorf("PTZ HTTP%d: %s", response.StatusCode, body)
		}
		ptzDone <- err
	}()
	select {
	case <-ptzStarted:
	case <-time.After(time.Second):
		t.Fatal("PTZ START not received")
	}
	cancel()
	deadline = time.Now().Add(time.Second)
	for {
		connection, err := net.DialTimeout("tcp", httpAddress, 20*time.Millisecond)
		if err != nil {
			break
		}
		_ = connection.Close()
		if time.Now().After(deadline) {
			t.Fatal("HTTP listener did not close during drain")
		}
		time.Sleep(5 * time.Millisecond)
	}
	// XML 屏障保持未释放；持续独立 Keepalive 证明 HTTP drain 期间 SIP 仍活着。
	observation := time.Now().Add(150 * time.Millisecond)
	interval := time.NewTicker(10 * time.Millisecond)
	defer interval.Stop()
	keepalives := 0
	for time.Now().Before(observation) {
		<-interval.C
		message := sip.NewRequest(sip.MESSAGE, sip.Uri{Scheme: "sip", User: "34020000002000000001", Host: "127.0.0.1"})
		message.AppendHeader(&sip.FromHeader{Address: identity, Params: sip.NewParams()})
		message.AppendHeader(&sip.ToHeader{Address: message.Recipient})
		message.AppendHeader(sip.NewHeader("Content-Type", "Application/MANSCDP+xml"))
		message.SetBody([]byte(fmt.Sprintf("<Notify><CmdType>Keepalive</CmdType><SN>%d</SN><DeviceID>%s</DeviceID><Status>OK</Status></Notify>", keepalives+100, queryTestDeviceID)))
		message.SetTransport("UDP")
		message.SetDestination(sipAddress)
		messageCtx, stop := context.WithTimeout(context.Background(), 200*time.Millisecond)
		answer, err := peerClient.Do(messageCtx, message)
		stop()
		if err != nil {
			t.Errorf("HTTP drain closed SIP before XML release: %v", err)
			break
		}
		if answer.StatusCode != sip.StatusOK {
			t.Errorf("Keepalive SIP %d", answer.StatusCode)
			break
		}
		keepalives++
	}
	t.Logf("HTTP listener closed while XML blocked: %d Keepalive SIP 200", keepalives)
	select {
	case <-ptzStopped:
	case <-time.After(2 * time.Second):
		t.Fatal("STOP not accepted during HTTP drain")
	}
	select {
	case err := <-ptzDone:
		if err != nil {
			t.Error(err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("PTZ HTTP did not drain")
	}
	releaseOnce.Do(func() { close(releaseXML) })
	select {
	case err := <-queryDone:
		if err != nil {
			t.Error(err)
		}
	case <-time.After(6 * time.Second):
		t.Fatal("query did not drain")
	}
}

func TestRunStartupFailureStopsOtherListener(t *testing.T) {
	for _, occupied := range []string{"tcp", "udp"} {
		t.Run(occupied, func(t *testing.T) {
			tcp, err := net.Listen("tcp", "127.0.0.1:0")
			if err != nil {
				t.Fatal(err)
			}
			defer tcp.Close()
			udp, err := net.ListenPacket("udp", "127.0.0.1:0")
			if err != nil {
				t.Fatal(err)
			}
			defer udp.Close()
			httpAddress, sipAddress := tcp.Addr().String(), udp.LocalAddr().String()
			if occupied == "tcp" {
				_ = udp.Close()
			} else {
				_ = tcp.Close()
			}
			done := make(chan error, 1)
			ctx, cancel := context.WithCancel(t.Context())
			defer cancel()
			go func() {
				done <- run(ctx, []string{"--database", filepath.Join(t.TempDir(), "failed.db"), "--sip-listen", sipAddress, "--sip-advertise", sipAddress, "--http-listen", httpAddress}, slog.New(slog.NewTextHandler(io.Discard, nil)))
			}()
			select {
			case err := <-done:
				if err == nil {
					t.Fatal("bind failure returned nil")
				}
			case <-time.After(3 * time.Second):
				t.Fatal("bind failure deadlocked")
			}
			if occupied == "tcp" {
				connection, err := net.ListenPacket("udp", sipAddress)
				if err != nil {
					t.Fatal(err)
				}
				_ = connection.Close()
			} else {
				listener, err := net.Listen("tcp", httpAddress)
				if err != nil {
					t.Fatal(err)
				}
				_ = listener.Close()
			}
		})
	}
}
