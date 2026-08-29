package main

import (
	"bytes"
	"context"
	"encoding/xml"
	"fmt"
	"io"
	"log/slog"
	"net"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"testing"
	"time"

	"github.com/emiago/sipgo/sip"
)

func TestGB28181LiveWithMediaServer(t *testing.T) {
	mediaBinary := os.Getenv("MEDIA_SERVER_E2E_BINARY")
	if mediaBinary == "" {
		t.Skip("MEDIA_SERVER_E2E_BINARY is not set")
	}
	fixtureBinary := filepath.Join(filepath.Dir(mediaBinary), "gb28181_rtp_fixture")
	if _, err := os.Stat(fixtureBinary); err != nil {
		t.Fatalf("GB28181 RTP fixture not found: %v", err)
	}
	ffmpeg, err := exec.LookPath("ffmpeg")
	if err != nil {
		t.Fatalf("ffmpeg not found: %v", err)
	}
	ffprobe, err := exec.LookPath("ffprobe")
	if err != nil {
		t.Fatalf("ffprobe not found: %v", err)
	}

	cfg := testConfig()
	platform, sipAddress := startRegistrar(t, cfg)
	mediaRegistry := newMediaServerRegistry()
	infrastructure := httptest.NewServer(newInfrastructureServer(cfg, mediaRegistry, slog.New(slog.NewTextHandler(io.Discard, nil))).handler())
	t.Cleanup(infrastructure.Close)

	listeners := make([]net.Listener, 3)
	ports := make([]int, len(listeners))
	for index := range listeners {
		listener, listenErr := net.Listen("tcp4", "127.0.0.1:0")
		if listenErr != nil {
			t.Fatalf("reserve TCP port: %v", listenErr)
		}
		listeners[index] = listener
		ports[index] = listener.Addr().(*net.TCPAddr).Port
	}
	for _, listener := range listeners {
		if closeErr := listener.Close(); closeErr != nil {
			t.Fatalf("release TCP port: %v", closeErr)
		}
	}
	rtmpPort, rtspPort, httpPort := ports[0], ports[1], ports[2]
	logFile, err := os.CreateTemp("", "media-server-signaling-e2e-*.log")
	if err != nil {
		t.Fatalf("create media server log: %v", err)
	}
	logPath := logFile.Name()
	command := exec.Command(
		mediaBinary,
		"--rtmp-port", strconv.Itoa(rtmpPort),
		"--rtsp-port", strconv.Itoa(rtspPort),
		"--http-port", strconv.Itoa(httpPort),
		"--threads", "2",
		"--signaling-url", infrastructure.URL,
		"--server-id", "media-e2e",
		"--control-url", fmt.Sprintf("http://127.0.0.1:%d", httpPort),
		"--media-ip", "127.0.0.1",
	)
	command.Stdout = logFile
	command.Stderr = logFile
	if err := command.Start(); err != nil {
		_ = logFile.Close()
		_ = os.Remove(logPath)
		t.Fatalf("start media server: %v", err)
	}
	processDone := make(chan struct{})
	var processErr error
	go func() {
		processErr = command.Wait()
		close(processDone)
	}()
	t.Cleanup(func() {
		select {
		case <-processDone:
		default:
			_ = command.Process.Signal(syscall.SIGTERM)
			select {
			case <-processDone:
			case <-time.After(5 * time.Second):
				_ = command.Process.Kill()
				<-processDone
			}
		}
		if processErr != nil {
			t.Errorf("media server exit: %v", processErr)
		}
		_ = logFile.Close()
		_ = os.Remove(logPath)
	})

	registrationDeadline := time.Now().Add(10 * time.Second)
	var mediaInstance mediaServerInstance
	for {
		if instance, online := mediaRegistry.selectOnline(); online {
			mediaInstance = instance
			break
		}
		select {
		case <-processDone:
			_ = logFile.Sync()
			logs, _ := os.ReadFile(logPath)
			t.Fatalf("media server exited before registration: %v\n%s", processErr, logs)
		default:
		}
		if time.Now().After(registrationDeadline) {
			_ = logFile.Sync()
			logs, _ := os.ReadFile(logPath)
			t.Fatalf("media server registration timeout\n%s", logs)
		}
		time.Sleep(20 * time.Millisecond)
	}

	device := startLiveTestDevice(t, func(request *sip.Request) ([]byte, int) {
		return []byte(strings.ReplaceAll(string(request.Body()), "a=recvonly", "a=sendonly")), sip.StatusOK
	})
	deviceHost, devicePortText, err := net.SplitHostPort(device.addr)
	if err != nil {
		t.Fatalf("split device address: %v", err)
	}
	devicePort, err := strconv.Atoi(devicePortText)
	if err != nil {
		t.Fatalf("parse device port: %v", err)
	}
	registerRequest := newRegisterRequest(t, cfg, sipAddress, testDeviceID, 120, deviceHost, devicePort)
	challenge := doSIP(t, device.client, registerRequest)
	registerResponse := doDigest(t, device.client, registerRequest, challenge, testDeviceID, cfg.sipPassword)
	if registerResponse.StatusCode != sip.StatusOK {
		t.Fatalf("REGISTER status = %d", registerResponse.StatusCode)
	}

	var catalogRequest *sip.Request
	select {
	case catalogRequest = <-device.messages:
	case <-time.After(2 * time.Second):
		t.Fatal("Catalog query was not received")
	}
	var query catalogQuery
	if err := xml.Unmarshal(catalogRequest.Body(), &query); err != nil {
		t.Fatalf("decode Catalog query: %v", err)
	}
	catalogResponse := doSIP(t, device.client, newMessageRequest(t, cfg, sipAddress, testDeviceID, catalogXML(query.SN, 1, []catalogChannel{{
		DeviceID: testChannelID,
		Name:     "e2e camera",
		ParentID: testDeviceID,
		Status:   "ON",
	}})))
	if catalogResponse.StatusCode != sip.StatusOK {
		t.Fatalf("Catalog response status = %d", catalogResponse.StatusCode)
	}
	keepaliveResponse := doSIP(t, device.client, newMessageRequest(t, cfg, sipAddress, testDeviceID, keepaliveXML(testDeviceID, "Keepalive", "OK")))
	if keepaliveResponse.StatusCode != sip.StatusOK {
		t.Fatalf("Keepalive status = %d", keepaliveResponse.StatusCode)
	}

	allocator, err := newSSRCAllocator(cfg.sipDomain)
	if err != nil {
		t.Fatalf("new SSRC allocator: %v", err)
	}
	live := newLiveService(platform, mediaRegistry, newMediaServerHTTPClient(cfg.mediaRequestTimeout), allocator, slog.New(slog.NewTextHandler(io.Discard, nil)))
	view, err := live.startLive(t.Context(), testDeviceID, testChannelID)
	if err != nil {
		t.Fatalf("start live: %v", err)
	}
	if view.state != liveStreaming || mediaInstance.mediaIP != "127.0.0.1" {
		t.Fatalf("live=%+v media=%+v", view, mediaInstance)
	}
	select {
	case invite := <-device.invites:
		body := string(invite.Body())
		if !strings.Contains(body, fmt.Sprintf("m=video %d RTP/AVP 96", view.rtpPort)) ||
			!strings.Contains(body, fmt.Sprintf("y=%010d", view.ssrc)) {
			t.Fatalf("INVITE SDP does not contain allocated endpoint:\n%s", body)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("INVITE was not received")
	}
	select {
	case <-device.acks:
	case <-time.After(2 * time.Second):
		t.Fatal("ACK was not received")
	}

	h264File, err := os.CreateTemp("", "gb28181-signaling-e2e-*.h264")
	if err != nil {
		t.Fatalf("create H264 fixture: %v", err)
	}
	h264Path := h264File.Name()
	if err := h264File.Close(); err != nil {
		t.Fatalf("close H264 fixture: %v", err)
	}
	t.Cleanup(func() { _ = os.Remove(h264Path) })
	fixtureContext, fixtureCancel := context.WithTimeout(t.Context(), 15*time.Second)
	fixture := exec.CommandContext(
		fixtureContext,
		ffmpeg,
		"-hide_banner", "-loglevel", "error",
		"-f", "lavfi", "-i", "testsrc2=size=320x240:rate=25",
		"-t", "8",
		"-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency",
		"-pix_fmt", "yuv420p", "-g", "25", "-keyint_min", "25", "-sc_threshold", "0", "-bf", "0",
		"-bsf:v", "h264_metadata=aud=insert", "-an", "-f", "h264", "-y", h264Path,
	)
	fixtureOutput, err := fixture.CombinedOutput()
	fixtureCancel()
	if err != nil {
		t.Fatalf("generate Annex-B H264 fixture: %v output=%s", err, fixtureOutput)
	}
	sendContext, stopSending := context.WithCancel(t.Context())
	sender := exec.CommandContext(
		sendContext,
		fixtureBinary,
		mediaInstance.mediaIP,
		strconv.Itoa(int(view.rtpPort)),
		"96",
		strconv.FormatUint(uint64(view.ssrc), 10),
		h264Path,
	)
	var senderOutput bytes.Buffer
	sender.Stdout = &senderOutput
	sender.Stderr = &senderOutput
	if err := sender.Start(); err != nil {
		stopSending()
		t.Fatalf("start RTP/PS fixture: %v", err)
	}
	senderDone := make(chan struct{})
	var senderErr error
	go func() {
		senderErr = sender.Wait()
		close(senderDone)
	}()
	cleanupSender := sync.OnceFunc(func() {
		stopSending()
		<-senderDone
	})
	t.Cleanup(cleanupSender)

	rtspURL := fmt.Sprintf("rtsp://127.0.0.1:%d/%s", rtspPort, view.streamName)
	probeDeadline := time.Now().Add(15 * time.Second)
	var probeOutput []byte
	var probeErr error
	for time.Now().Before(probeDeadline) {
		attemptContext, attemptCancel := context.WithTimeout(t.Context(), 3*time.Second)
		probe := exec.CommandContext(
			attemptContext,
			ffprobe,
			"-v", "error", "-rtsp_transport", "tcp",
			"-analyzeduration", "2000000", "-probesize", "1000000",
			"-select_streams", "v:0", "-show_entries", "stream=codec_name",
			"-of", "default=noprint_wrappers=1:nokey=1",
			rtspURL,
		)
		probeOutput, probeErr = probe.CombinedOutput()
		attemptCancel()
		if probeErr == nil && strings.TrimSpace(string(probeOutput)) == "h264" {
			break
		}
		time.Sleep(100 * time.Millisecond)
	}
	if probeErr != nil || strings.TrimSpace(string(probeOutput)) != "h264" {
		cleanupSender()
		_ = logFile.Sync()
		logs, _ := os.ReadFile(logPath)
		t.Fatalf("ffprobe did not parse H264 from %s: %v output=%q sender=%v %q\n%s",
			rtspURL, probeErr, probeOutput, senderErr, senderOutput.String(), logs)
	}

	cleanupSender()
	if err := live.stopLive(t.Context(), testDeviceID, testChannelID); err != nil {
		t.Fatalf("stop live: %v", err)
	}
	select {
	case <-device.byes:
	case <-time.After(2 * time.Second):
		t.Fatal("BYE was not received")
	}
	if live.len() != 0 || allocator.activeCount() != 0 {
		t.Fatalf("live cleanup sessions=%d SSRCs=%d", live.len(), allocator.activeCount())
	}
}
