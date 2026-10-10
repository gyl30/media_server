package main

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"net/url"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"
)

func TestPlaySelectsExactlyOneSource(t *testing.T) {
	s := testInfrastructure(t)
	id := "11111111-1111-4111-8111-111111111111"
	for _, body := range []string{
		`{}`, `{"source":{}}`, `{"source":{"type":"gb","id":"` + id + `"}}`,
		`{"source":{"live_id":"invalid"}}`,
		`{"source":{"live_id":"` + id + `","source_id":"` + id + `"}}`,
		`{"source":{"live_id":"` + id + `","push_device_id":"` + id + `"}}`,
	} {
		apiRequest(t, s.handler(), "POST", "/api/play", body, http.StatusBadRequest)
	}
}

func TestRTSPTerminationInvalidatesPlaybackTokens(t *testing.T) {
	for _, action := range []string{"stop", "remove", "reconcile", "shutdown"} {
		t.Run(action, func(t *testing.T) {
			s := testInfrastructure(t)
			media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) { writer.WriteHeader(http.StatusNoContent) }))
			t.Cleanup(media.Close)
			s.media.server.controlURL = media.URL
			sourceID := "11111111-1111-4111-8111-111111111111"
			session, err := s.reserveRTSPPull(sourceID, "Camera")
			if err != nil || !s.finishRTSPPull(session) {
				t.Fatalf("reserve/finish: %v", err)
			}
			result, err := s.newPlaybackURLs(playSource{SourceID: sourceID})
			if err != nil {
				t.Fatal(err)
			}
			switch action {
			case "stop":
				if _, _, _, err := s.beginRTSPPullStop(sourceID); err != nil {
					t.Fatal(err)
				}
			case "remove":
				s.removeRTSPPull(session)
			case "reconcile":
				s.dropEndedRTSPPull(session)
			case "shutdown":
				s.shutdownRTSPPulls(context.Background())
			}
			verifyToken(t, s, result.Token, "play", result.StreamID, http.StatusForbidden)
			if _, ok := s.tokens.tokens[result.Token]; ok {
				t.Fatal("terminated source retained token")
			}
		})
	}
}

func TestPushDevicesPersistWithoutRuntimeState(t *testing.T) {
	path := filepath.Join(t.TempDir(), "sources.db")
	store, err := openSourceStore(context.Background(), path)
	if err != nil {
		t.Fatal(err)
	}
	s := testInfrastructure(t)
	s.sources = store
	issuedID, verifiedID := createPushDevice(t, s), createPushDevice(t, s)
	requestPushToken(t, s, issuedID)
	verified := requestPushToken(t, s, verifiedID)
	verifyToken(t, s, verified, "publish", verified, http.StatusOK)
	if _, err := s.newPlaybackURLs(playSource{PushDeviceID: verifiedID}); err != nil {
		t.Fatal(err)
	}
	if err := store.close(); err != nil {
		t.Fatal(err)
	}
	store, err = openSourceStore(context.Background(), path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = store.close() })
	restarted := newInfrastructureServer("127.0.0.1:0", store, s.live, s.media, s.logger)
	for _, id := range []string{issuedID, verifiedID} {
		restored, err := store.getPushDevice(context.Background(), id)
		if err != nil || restored.deviceID != id || restored.name != "Camera" {
			t.Fatalf("restored device=%+v err=%v", restored, err)
		}
		if response := restarted.pushDeviceResponseLocked(restored); response.State != "idle" || response.Token != "" {
			t.Fatalf("runtime restored: %+v", response)
		}
	}
	if len(restarted.tokens.tokens) != 0 || len(restarted.tokens.pushRuns) != 0 {
		t.Fatal("restart restored runtime tokens/runs")
	}
}

func TestPushDatabaseFailureKeepsCurrentRun(t *testing.T) {
	s := testInfrastructure(t)
	deviceID := createPushDevice(t, s)
	value := requestPushToken(t, s, deviceID)
	if err := s.sources.close(); err != nil {
		t.Fatal(err)
	}
	if err := s.stopPushDevice(context.Background(), deviceID, true); err == nil {
		t.Fatal("closed database delete succeeded")
	}
	if s.tokens.pushRuns[deviceID].streamID != value {
		t.Fatal("failed delete removed active run")
	}
	if _, ok := s.tokens.tokens[value]; !ok {
		t.Fatal("failed delete revoked publication token")
	}
}

type blockedTokenResponse struct {
	*httptest.ResponseRecorder
	entered chan struct{}
	release chan struct{}
	once    sync.Once
}

func (writer *blockedTokenResponse) WriteHeader(status int) {
	writer.once.Do(func() {
		close(writer.entered)
		<-writer.release
	})
	writer.ResponseRecorder.WriteHeader(status)
}

func (writer *blockedTokenResponse) Write(data []byte) (int, error) {
	writer.once.Do(func() {
		close(writer.entered)
		<-writer.release
	})
	return writer.ResponseRecorder.Write(data)
}

func TestTokenResponsesDoNotHoldStateLocks(t *testing.T) {
	for _, operation := range []string{"list", "get", "patch", "publish", "verify", "verify_rejected"} {
		t.Run(operation, func(t *testing.T) {
			s := testInfrastructure(t)
			deviceID := createPushDevice(t, s)
			method, path, body := "GET", "/api/push-devices", ""
			switch operation {
			case "get":
				path += "/" + deviceID
			case "patch":
				method, path, body = "PATCH", path+"/"+deviceID, `{"name":"Renamed"}`
			case "publish":
				method, path = "POST", path+"/"+deviceID+"/publish"
			case "verify", "verify_rejected":
				value := requestPushToken(t, s, deviceID)
				streamID := value
				if operation == "verify_rejected" {
					streamID = "different-source"
				}
				method, path = "POST", "/internal/verify"
				body = `{"token":"` + value + `","operation":"publish","stream_id":"` + streamID + `"}`
			}
			request := httptest.NewRequest(method, path, strings.NewReader(body))
			request.RemoteAddr = "127.0.0.1:12345"
			request.Header.Set("Content-Type", "application/json")
			writer := &blockedTokenResponse{ResponseRecorder: httptest.NewRecorder(), entered: make(chan struct{}), release: make(chan struct{})}
			done := make(chan struct{})
			go func() { s.handler().ServeHTTP(writer, request); close(done) }()
			select {
			case <-writer.entered:
			case <-time.After(time.Second):
				close(writer.release)
				t.Fatal("handler did not begin response")
			}
			var held []string
			for name, mutex := range map[string]*sync.Mutex{"tokens": &s.tokens.mu, "sources": &s.sourceOperationMu, "lives": &s.live.mu, "rtsp": &s.rtspPullMu} {
				if mutex.TryLock() {
					mutex.Unlock()
				} else {
					held = append(held, name)
				}
			}
			close(writer.release)
			select {
			case <-done:
			case <-time.After(time.Second):
				t.Fatal("response did not complete")
			}
			if len(held) != 0 {
				t.Fatalf("response held state locks: %v", held)
			}
		})
	}
}

func createPushDevice(t *testing.T, s *infrastructureServer) string {
	t.Helper()
	response := apiRequest(t, s.handler(), "POST", "/api/push-devices", `{"name":"Camera"}`, http.StatusCreated)
	var device pushDeviceResponse
	if err := json.Unmarshal(response.Body.Bytes(), &device); err != nil || !validUUIDv4(device.DeviceID) || device.State != "idle" {
		t.Fatalf("invalid push device: %s", response.Body.String())
	}
	return device.DeviceID
}

func requestPushToken(t *testing.T, s *infrastructureServer, deviceID string) string {
	t.Helper()
	response := apiRequest(t, s.handler(), "POST", "/api/push-devices/"+deviceID+"/publish", "", http.StatusCreated)
	var result struct {
		Token string `json:"token"`
	}
	if err := json.Unmarshal(response.Body.Bytes(), &result); err != nil || !validStreamToken(result.Token) {
		t.Fatalf("invalid push token: %s", response.Body.String())
	}
	if strings.Contains(response.Body.String(), `"stream_id"`) {
		t.Fatalf("push token must be the only source identity: %s", response.Body.String())
	}
	return result.Token
}

func TestPushDeviceRegistrationAndPublishAuthorization(t *testing.T) {
	s := testInfrastructure(t)
	deviceID := createPushDevice(t, s)
	value := requestPushToken(t, s, deviceID)
	apiRequest(t, s.handler(), "POST", "/api/push-devices/"+deviceID+"/publish", "", http.StatusConflict)
	apiRequest(t, s.handler(), "POST", "/api/play", `{"source":{"push_device_id":"`+deviceID+`"}}`, http.StatusConflict)
	verifyToken(t, s, value, "play", value, http.StatusForbidden)
	verifyToken(t, s, value, "publish", "wrong-id", http.StatusForbidden)
	verifyToken(t, s, value, "publish", value, http.StatusOK)
	verifyToken(t, s, value, "publish", value, http.StatusForbidden)
	apiRequest(t, s.handler(), "POST", "/api/push-devices/"+deviceID+"/publish", "", http.StatusConflict)
	play := apiRequest(t, s.handler(), "POST", "/api/play", `{"source":{"push_device_id":"`+deviceID+`"}}`, http.StatusCreated)
	var result playbackURLs
	if err := json.Unmarshal(play.Body.Bytes(), &result); err != nil || result.StreamID != value || !validStreamToken(result.Token) {
		t.Fatalf("invalid play: %s", play.Body.String())
	}
	verifyToken(t, s, result.Token, "play", value, http.StatusOK)
	device := apiRequest(t, s.handler(), "PATCH", "/api/push-devices/"+deviceID, `{"name":"Renamed"}`, http.StatusOK)
	if !strings.Contains(device.Body.String(), `"state":"verified"`) || !strings.Contains(device.Body.String(), `"name":"Renamed"`) {
		t.Fatal(device.Body.String())
	}
	apiRequest(t, s.handler(), "GET", "/api/push-devices", "", http.StatusOK)
}

func TestConcurrentVerificationConsumesExactlyOnce(t *testing.T) {
	s := testInfrastructure(t)
	deviceID := createPushDevice(t, s)
	value := requestPushToken(t, s, deviceID)
	body := `{"token":"` + value + `","operation":"publish","stream_id":"` + value + `"}`
	var wait sync.WaitGroup
	results := make(chan int, 16)
	for range 16 {
		wait.Go(func() {
			request := httptest.NewRequest("POST", "/internal/verify", strings.NewReader(body))
			request.RemoteAddr = "127.0.0.1:12345"
			request.Header.Set("Content-Type", "application/json")
			response := httptest.NewRecorder()
			s.handler().ServeHTTP(response, request)
			results <- response.Code
		})
	}
	wait.Wait()
	close(results)
	successes := 0
	for status := range results {
		if status == http.StatusOK {
			successes++
		} else if status != http.StatusForbidden {
			t.Fatalf("unexpected verify status: %d", status)
		}
	}
	if successes != 1 {
		t.Fatalf("successful verifications=%d", successes)
	}
}

func TestTokenExpiryDoesNotExpireVerifiedRun(t *testing.T) {
	s := testInfrastructure(t)
	s.tokenTTL = time.Second
	deviceID := createPushDevice(t, s)
	first := requestPushToken(t, s, deviceID)
	expiresAt := s.tokens.tokens[first].expiresAt
	s.live.sip.now = func() time.Time { return expiresAt }
	verifyToken(t, s, first, "publish", first, http.StatusForbidden)
	second := requestPushToken(t, s, deviceID)
	if second == first {
		t.Fatal("expired push identity reused")
	}
	verifyToken(t, s, second, "publish", second, http.StatusOK)
	s.tokens.expire(expiresAt.Add(time.Hour))
	device := apiRequest(t, s.handler(), "GET", "/api/push-devices/"+deviceID, "", http.StatusOK)
	if !strings.Contains(device.Body.String(), `"state":"verified"`) {
		t.Fatalf("TTL ended an admitted run: %s", device.Body.String())
	}
}

func TestPushStopAndDeleteInvalidateAllRunTokens(t *testing.T) {
	for _, operation := range []string{"stop", "delete"} {
		t.Run(operation, func(t *testing.T) {
			s := testInfrastructure(t)
			var deletedID string
			media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
				var body struct {
					StreamID string `json:"stream_id"`
				}
				if request.URL.Path != "/receivers/delete" || json.NewDecoder(request.Body).Decode(&body) != nil {
					t.Errorf("invalid media stop: %s", request.URL.Path)
				}
				deletedID = body.StreamID
				writer.WriteHeader(http.StatusNoContent)
			}))
			t.Cleanup(media.Close)
			s.media.server.controlURL = media.URL
			deviceID := createPushDevice(t, s)
			value := requestPushToken(t, s, deviceID)
			verifyToken(t, s, value, "publish", value, http.StatusOK)
			play, err := s.newPlaybackURLs(playSource{PushDeviceID: deviceID})
			if err != nil {
				t.Fatal(err)
			}
			if operation == "delete" {
				apiRequest(t, s.handler(), "DELETE", "/api/push-devices/"+deviceID, "", http.StatusNoContent)
				apiRequest(t, s.handler(), "GET", "/api/push-devices/"+deviceID, "", http.StatusNotFound)
			} else {
				apiRequest(t, s.handler(), "POST", "/api/push-devices/"+deviceID+"/stop", "", http.StatusNoContent)
				requestPushToken(t, s, deviceID)
			}
			verifyToken(t, s, play.Token, "play", value, http.StatusForbidden)
			if deletedID != value {
				t.Fatalf("stopped wrong run: %s want %s", deletedID, value)
			}
		})
	}
}

func TestVerifyUsesServiceAuthenticationNotForwardedAddress(t *testing.T) {
	for _, item := range []struct {
		name, secret, address, authorization string
		status                               int
	}{
		{"loopback", "", "127.0.0.1:123", "", http.StatusOK},
		{"ipv6 loopback", "", "[::1]:123", "", http.StatusOK},
		{"external", "", "192.0.2.1:123", "", http.StatusForbidden},
		{"missing bearer", "secret", "127.0.0.1:123", "", http.StatusForbidden},
		{"wrong bearer", "secret", "127.0.0.1:123", "Bearer wrong", http.StatusForbidden},
		{"bearer", "secret", "192.0.2.1:123", "Bearer secret", http.StatusOK},
	} {
		t.Run(item.name, func(t *testing.T) {
			s := testInfrastructure(t)
			s.media.server.controlToken = item.secret
			deviceID := createPushDevice(t, s)
			value := requestPushToken(t, s, deviceID)
			body := `{"token":"` + value + `","operation":"publish","stream_id":"` + value + `"}`
			request := httptest.NewRequest("POST", "/internal/verify", strings.NewReader(body))
			request.RemoteAddr = item.address
			request.Header.Set("Content-Type", "application/json")
			request.Header.Set("Authorization", item.authorization)
			request.Header.Set("X-Forwarded-For", "127.0.0.1")
			response := httptest.NewRecorder()
			s.handler().ServeHTTP(response, request)
			if response.Code != item.status {
				t.Fatalf("HTTP=%d want=%d", response.Code, item.status)
			}
			if item.status != http.StatusOK {
				verifyToken(t, s, value, "publish", value, http.StatusOK)
			}
		})
	}
}

func TestPlaybackURLsUseRealPortsAndIPv6(t *testing.T) {
	s := testInfrastructure(t)
	s.media.server.mediaIP = "2001:db8::10"
	s.media.server.httpPort = 18080
	s.media.server.rtmpPort = 11935
	s.media.server.rtspPort = 18554
	deviceID := createPushDevice(t, s)
	value := requestPushToken(t, s, deviceID)
	verifyToken(t, s, value, "publish", value, http.StatusOK)
	play, err := s.newPlaybackURLs(playSource{PushDeviceID: deviceID})
	if err != nil {
		t.Fatal(err)
	}
	for _, item := range []struct{ value, scheme, host, path string }{
		{play.RTMPURL, "rtmp", "[2001:db8::10]:11935", "/" + value + "/" + play.Token},
		{play.RTSPURL, "rtsp", "[2001:db8::10]:18554", "/" + value + "/" + play.Token},
		{play.FLVURL, "http", "[2001:db8::10]:18080", "/" + value + "/" + play.Token + ".flv"},
		{play.HLSURL, "http", "[2001:db8::10]:18080", "/play/hls/" + value + "/" + play.Token + "/index.m3u8"},
		{play.WHEPURL, "http", "[2001:db8::10]:18080", "/play/whep/" + value + "/" + play.Token},
	} {
		parsed, err := url.Parse(item.value)
		if err != nil || parsed.Scheme != item.scheme || parsed.Host != item.host || parsed.Path != item.path || parsed.RawQuery != "" || parsed.Fragment != "" {
			t.Fatalf("invalid URL: %s err=%v", item.value, err)
		}
	}
}

func TestTokenConfigRejectsInvalidPortsAndLifetime(t *testing.T) {
	for _, args := range [][]string{
		{"--media-rtmp-port", "0"}, {"--media-rtsp-port", "65536"}, {"--token-ttl", "0s"}, {"--token-ttl", "-1s"},
	} {
		if _, err := parseConfig(args); err == nil {
			t.Fatalf("invalid config accepted: %v", args)
		}
	}
	cfg, err := parseConfig([]string{"--media-rtmp-port", "11935", "--media-rtsp-port", "18554", "--token-ttl", "2s"})
	if err != nil || cfg.mediaServer.rtmpPort != 11935 || cfg.mediaServer.rtspPort != 18554 || cfg.tokenTTL != 2*time.Second {
		t.Fatalf("config: %+v err=%v", cfg, err)
	}
}
