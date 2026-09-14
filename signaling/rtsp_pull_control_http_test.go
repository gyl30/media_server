package main

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

func TestRTSPPullControlCreateDeleteAndSelectMediaServer(t *testing.T) {
	type receivedRequest struct {
		path string
		body map[string]any
	}
	requests := make(chan receivedRequest, 4)
	mediaServer := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		var body map[string]any
		if err := json.NewDecoder(request.Body).Decode(&body); err != nil {
			t.Errorf("Decode() error = %v", err)
		}
		requests <- receivedRequest{path: request.URL.Path, body: body}
		status := http.StatusOK
		if request.URL.Path == "/rtsp/pull/create" {
			status = http.StatusCreated
		}
		writeJSON(writer, status, map[string]string{"result": "ok"})
	}))
	defer mediaServer.Close()

	registry := newMediaServerRegistry()
	registration := mediaServerRegistration{
		ServerID: "media-1", InstanceID: "instance-a", ControlURL: mediaServer.URL, MediaIP: "127.0.0.1",
	}
	if err := registry.register(registration, time.Now()); err != nil {
		t.Fatalf("register() error = %v", err)
	}
	server := newInfrastructureServer(testConfig(), registry, slog.New(slog.NewTextHandler(io.Discard, nil)))
	httpServer := httptest.NewServer(server.handler())
	defer httpServer.Close()

	username := "admin"
	password := "123456"
	command := rtspPullCreateRequest{
		StreamName: "live/camera", URL: "rtsp://192.0.2.10/live", Username: &username, Password: &password,
	}
	response := postJSON(t, httpServer.Client(), httpServer.URL+"/internal/rtsp-pull/create", command)
	if response.StatusCode != http.StatusCreated {
		t.Fatalf("create status = %d body = %s", response.StatusCode, readBody(t, response))
	}
	response.Body.Close()
	create := <-requests
	if create.path != "/rtsp/pull/create" || len(create.body) != 4 || create.body["stream_name"] != command.StreamName ||
		create.body["url"] != command.URL || create.body["username"] != username || create.body["password"] != password {
		t.Fatalf("create request = %#v", create)
	}

	response = postJSON(t, httpServer.Client(), httpServer.URL+"/internal/rtsp-pull/delete", map[string]string{"stream_name": command.StreamName})
	if response.StatusCode != http.StatusOK {
		t.Fatalf("delete status = %d body = %s", response.StatusCode, readBody(t, response))
	}
	response.Body.Close()
	remove := <-requests
	if remove.path != "/rtsp/pull/delete" || len(remove.body) != 1 || remove.body["stream_name"] != command.StreamName {
		t.Fatalf("delete request = %#v", remove)
	}

	response = postJSON(t, httpServer.Client(), httpServer.URL+"/internal/rtsp-pull/delete", map[string]string{"stream_name": command.StreamName})
	assertHTTPError(t, response, http.StatusNotFound, "not_found")
}

func TestRTSPPullControlAllowsRecreateAfterRuntimeFailure(t *testing.T) {
	var creates atomic.Int32
	mediaServer := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		if request.URL.Path != "/rtsp/pull/create" {
			http.NotFound(writer, request)
			return
		}
		creates.Add(1)
		writeJSON(writer, http.StatusCreated, map[string]string{"result": "ok"})
	}))
	defer mediaServer.Close()

	registry := newMediaServerRegistry()
	if err := registry.register(mediaServerRegistration{
		ServerID: "media-1", InstanceID: "instance-a", ControlURL: mediaServer.URL, MediaIP: "127.0.0.1",
	}, time.Now()); err != nil {
		t.Fatalf("register() error = %v", err)
	}
	server := newInfrastructureServer(testConfig(), registry, slog.New(slog.NewTextHandler(io.Discard, nil)))
	httpServer := httptest.NewServer(server.handler())
	defer httpServer.Close()
	command := rtspPullCreateRequest{StreamName: "live/recreate", URL: "rtsp://192.0.2.10/live"}
	for range 2 {
		response := postJSON(t, httpServer.Client(), httpServer.URL+"/internal/rtsp-pull/create", command)
		if response.StatusCode != http.StatusCreated {
			t.Fatalf("create status = %d body = %s", response.StatusCode, readBody(t, response))
		}
		response.Body.Close()
	}
	if creates.Load() != 2 {
		t.Fatalf("creates = %d", creates.Load())
	}
}

func TestRTSPPullControlDelayedDeletePreservesReplacement(t *testing.T) {
	deleteStarted := make(chan struct{})
	releaseDelete := make(chan struct{})
	var deletes atomic.Int32
	mediaServer := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		switch request.URL.Path {
		case "/rtsp/pull/create":
			writeJSON(writer, http.StatusCreated, map[string]string{"result": "ok"})
		case "/rtsp/pull/delete":
			if deletes.Add(1) == 1 {
				close(deleteStarted)
				<-releaseDelete
			}
			writeJSON(writer, http.StatusOK, map[string]string{"result": "ok"})
		default:
			http.NotFound(writer, request)
		}
	}))
	defer mediaServer.Close()

	registry := newMediaServerRegistry()
	if err := registry.register(mediaServerRegistration{
		ServerID: "media-1", InstanceID: "instance-a", ControlURL: mediaServer.URL, MediaIP: "127.0.0.1",
	}, time.Now()); err != nil {
		t.Fatalf("register() error = %v", err)
	}
	server := newInfrastructureServer(testConfig(), registry, slog.New(slog.NewTextHandler(io.Discard, nil)))
	command := rtspPullCreateRequest{StreamName: "live/replacement", URL: "rtsp://192.0.2.10/live"}
	createRequest := httptest.NewRequest(http.MethodPost, "/internal/rtsp-pull/create", strings.NewReader(`{"stream_name":"live/replacement","url":"rtsp://192.0.2.10/live"}`))
	createRequest.Header.Set("Content-Type", "application/json")
	createResponse := httptest.NewRecorder()
	server.handleRTSPPullCreate(createResponse, createRequest)
	if createResponse.Code != http.StatusCreated {
		t.Fatalf("initial create response = %d %s", createResponse.Code, createResponse.Body.String())
	}

	deleteRequest := httptest.NewRequest(http.MethodPost, "/internal/rtsp-pull/delete", strings.NewReader(`{"stream_name":"live/replacement"}`))
	deleteRequest.Header.Set("Content-Type", "application/json")
	deleteResponse := httptest.NewRecorder()
	deleteDone := make(chan struct{})
	go func() {
		server.handleRTSPPullDelete(deleteResponse, deleteRequest)
		close(deleteDone)
	}()
	select {
	case <-deleteStarted:
	case <-time.After(time.Second):
		t.Fatal("delete did not reach media server")
	}

	recreateRequest := httptest.NewRequest(http.MethodPost, "/internal/rtsp-pull/create", strings.NewReader(`{"stream_name":"live/replacement","url":"rtsp://192.0.2.10/live"}`))
	recreateRequest.Header.Set("Content-Type", "application/json")
	recreateResponse := httptest.NewRecorder()
	server.handleRTSPPullCreate(recreateResponse, recreateRequest)
	if recreateResponse.Code != http.StatusCreated {
		t.Fatalf("recreate response = %d %s", recreateResponse.Code, recreateResponse.Body.String())
	}
	close(releaseDelete)
	select {
	case <-deleteDone:
	case <-time.After(time.Second):
		t.Fatal("delete did not finish")
	}
	if deleteResponse.Code != http.StatusOK {
		t.Fatalf("delete response = %d %s", deleteResponse.Code, deleteResponse.Body.String())
	}

	finalDeleteRequest := httptest.NewRequest(http.MethodPost, "/internal/rtsp-pull/delete", strings.NewReader(`{"stream_name":"live/replacement"}`))
	finalDeleteRequest.Header.Set("Content-Type", "application/json")
	finalDeleteResponse := httptest.NewRecorder()
	server.handleRTSPPullDelete(finalDeleteResponse, finalDeleteRequest)
	if finalDeleteResponse.Code != http.StatusOK || deletes.Load() != 2 {
		t.Fatalf("replacement delete = %d %s calls=%d command=%s", finalDeleteResponse.Code, finalDeleteResponse.Body.String(), deletes.Load(), command.StreamName)
	}
}

func TestRTSPPullControlValidatesAndMapsMediaErrors(t *testing.T) {
	mediaServer := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		var command rtspPullCreateRequest
		if err := json.NewDecoder(request.Body).Decode(&command); err != nil {
			t.Errorf("Decode() error = %v", err)
		}
		switch command.StreamName {
		case "live/invalid":
			writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		case "live/conflict":
			writeHTTPError(writer, http.StatusConflict, "conflict")
		default:
			writeJSON(writer, http.StatusCreated, map[string]string{"result": "ok"})
		}
	}))
	defer mediaServer.Close()

	registry := newMediaServerRegistry()
	if err := registry.register(mediaServerRegistration{
		ServerID: "media-1", InstanceID: "instance-a", ControlURL: mediaServer.URL, MediaIP: "127.0.0.1",
	}, time.Now()); err != nil {
		t.Fatalf("register() error = %v", err)
	}
	server := newInfrastructureServer(testConfig(), registry, slog.New(slog.NewTextHandler(io.Discard, nil)))
	httpServer := httptest.NewServer(server.handler())
	defer httpServer.Close()

	for name, body := range map[string]string{
		"invalid JSON":     `{`,
		"missing stream":   `{"url":"rtsp://192.0.2.10/live"}`,
		"missing URL":      `{"stream_name":"live/camera"}`,
		"password only":    `{"stream_name":"live/camera","url":"rtsp://192.0.2.10/live","password":"secret"}`,
		"empty username":   `{"stream_name":"live/camera","url":"rtsp://192.0.2.10/live","username":"","password":""}`,
		"null username":    `{"stream_name":"live/camera","url":"rtsp://192.0.2.10/live","username":null}`,
		"null password":    `{"stream_name":"live/camera","url":"rtsp://192.0.2.10/live","username":"admin","password":null}`,
		"unknown field":    `{"stream_name":"live/camera","url":"rtsp://192.0.2.10/live","control_url":"http://other"}`,
		"wrong field type": `{"stream_name":1,"url":"rtsp://192.0.2.10/live"}`,
	} {
		t.Run(name, func(t *testing.T) {
			request := httptest.NewRequest(http.MethodPost, "/internal/rtsp-pull/create", strings.NewReader(body))
			request.Header.Set("Content-Type", "application/json")
			recorder := httptest.NewRecorder()
			server.handleRTSPPullCreate(recorder, request)
			if recorder.Code != http.StatusBadRequest || recorder.Body.String() != "{\"error\":\"invalid_request\"}\n" {
				t.Fatalf("response = %d %s", recorder.Code, recorder.Body.String())
			}
		})
	}

	for streamName, expectedStatus := range map[string]int{
		"live/invalid":  http.StatusBadRequest,
		"live/conflict": http.StatusConflict,
	} {
		response := postJSON(t, httpServer.Client(), httpServer.URL+"/internal/rtsp-pull/create", rtspPullCreateRequest{
			StreamName: streamName, URL: "rtsp://192.0.2.10/live",
		})
		if response.StatusCode != expectedStatus {
			t.Fatalf("%s status = %d body = %s", streamName, response.StatusCode, readBody(t, response))
		}
		response.Body.Close()
	}

	emptyServer := newInfrastructureServer(testConfig(), newMediaServerRegistry(), slog.New(slog.NewTextHandler(io.Discard, nil)))
	request := httptest.NewRequest(http.MethodPost, "/internal/rtsp-pull/create", strings.NewReader(`{"stream_name":"live/camera","url":"rtsp://192.0.2.10/live"}`))
	request.Header.Set("Content-Type", "application/json")
	recorder := httptest.NewRecorder()
	emptyServer.handleRTSPPullCreate(recorder, request)
	if recorder.Code != http.StatusServiceUnavailable || recorder.Body.String() != "{\"error\":\"no_media_server\"}\n" {
		t.Fatalf("no media server response = %d %s", recorder.Code, recorder.Body.String())
	}
}

func TestRTSPPullControlMapsFailuresWithoutLoggingPassword(t *testing.T) {
	mediaServer := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, _ *http.Request) {
		writeHTTPError(writer, http.StatusInternalServerError, "operation_failed")
	}))
	defer mediaServer.Close()

	registry := newMediaServerRegistry()
	if err := registry.register(mediaServerRegistration{
		ServerID: "media-1", InstanceID: "instance-a", ControlURL: mediaServer.URL, MediaIP: "127.0.0.1",
	}, time.Now()); err != nil {
		t.Fatalf("register() error = %v", err)
	}
	var logs bytes.Buffer
	server := newInfrastructureServer(testConfig(), registry, slog.New(slog.NewTextHandler(&logs, nil)))
	httpServer := httptest.NewServer(server.handler())
	defer httpServer.Close()
	username := "admin"
	password := "do-not-log-this-password"
	response := postJSON(t, httpServer.Client(), httpServer.URL+"/internal/rtsp-pull/create", rtspPullCreateRequest{
		StreamName: "live/failure", URL: "rtsp://192.0.2.10/live", Username: &username, Password: &password,
	})
	assertHTTPError(t, response, http.StatusBadGateway, "rtsp_pull_create_failed")
	if strings.Contains(logs.String(), password) {
		t.Fatalf("password leaked in logs: %s", logs.String())
	}
}

func TestRTSPPullControlDoesNotDeleteOnAmbiguousCreateFailure(t *testing.T) {
	var deletes atomic.Int32
	mediaServer := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		switch request.URL.Path {
		case "/rtsp/pull/create":
			writeJSON(writer, http.StatusCreated, map[string]string{"result": "unexpected"})
		case "/rtsp/pull/delete":
			deletes.Add(1)
			writeJSON(writer, http.StatusOK, map[string]string{"result": "ok"})
		default:
			http.NotFound(writer, request)
		}
	}))
	defer mediaServer.Close()

	registry := newMediaServerRegistry()
	if err := registry.register(mediaServerRegistration{
		ServerID: "media-1", InstanceID: "instance-a", ControlURL: mediaServer.URL, MediaIP: "127.0.0.1",
	}, time.Now()); err != nil {
		t.Fatalf("register() error = %v", err)
	}
	server := newInfrastructureServer(testConfig(), registry, slog.New(slog.NewTextHandler(io.Discard, nil)))
	server.rtspPulls["live/ambiguous"] = mediaServerInstance{
		serverID: "media-1", instanceID: "instance-a", controlURL: mediaServer.URL, online: true,
	}
	request := httptest.NewRequest(http.MethodPost, "/internal/rtsp-pull/create", strings.NewReader(`{"stream_name":"live/ambiguous","url":"rtsp://192.0.2.10/live"}`))
	request.Header.Set("Content-Type", "application/json")
	recorder := httptest.NewRecorder()
	server.handleRTSPPullCreate(recorder, request)
	if recorder.Code != http.StatusBadGateway || deletes.Load() != 0 {
		t.Fatalf("response = %d %s deletes=%d", recorder.Code, recorder.Body.String(), deletes.Load())
	}
	if mapped, ok := server.rtspPullServer("live/ambiguous"); !ok || mapped.instanceID != "instance-a" {
		t.Fatal("ambiguous create failure removed existing ownership")
	}
}

func TestRTSPPullControlCreateDoesNotRetainExpiredMediaServer(t *testing.T) {
	createStarted := make(chan struct{})
	releaseCreate := make(chan struct{})
	var deletes atomic.Int32
	mediaServer := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		switch request.URL.Path {
		case "/rtsp/pull/create":
			close(createStarted)
			<-releaseCreate
			writeJSON(writer, http.StatusCreated, map[string]string{"result": "ok"})
		case "/rtsp/pull/delete":
			deletes.Add(1)
			writeJSON(writer, http.StatusOK, map[string]string{"result": "ok"})
		default:
			http.NotFound(writer, request)
		}
	}))
	defer mediaServer.Close()

	registry := newMediaServerRegistry()
	now := time.Now()
	if err := registry.register(mediaServerRegistration{
		ServerID: "media-1", InstanceID: "instance-a", ControlURL: mediaServer.URL, MediaIP: "127.0.0.1",
	}, now); err != nil {
		t.Fatalf("register() error = %v", err)
	}
	server := newInfrastructureServer(testConfig(), registry, slog.New(slog.NewTextHandler(io.Discard, nil)))
	request := httptest.NewRequest(http.MethodPost, "/internal/rtsp-pull/create", strings.NewReader(`{"stream_name":"live/expired","url":"rtsp://192.0.2.10/live"}`))
	request.Header.Set("Content-Type", "application/json")
	recorder := httptest.NewRecorder()
	done := make(chan struct{})
	go func() {
		server.handleRTSPPullCreate(recorder, request)
		close(done)
	}()
	select {
	case <-createStarted:
	case <-time.After(time.Second):
		t.Fatal("create did not reach media server")
	}
	offline := registry.expire(now.Add(time.Hour), time.Minute)
	if len(offline) != 1 {
		t.Fatalf("expired servers = %d", len(offline))
	}
	server.removeRTSPPullsForMediaServer(offline[0])
	close(releaseCreate)
	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("create did not finish")
	}
	if recorder.Code != http.StatusServiceUnavailable || recorder.Body.String() != "{\"error\":\"no_media_server\"}\n" || deletes.Load() != 1 {
		t.Fatalf("response = %d %s deletes=%d", recorder.Code, recorder.Body.String(), deletes.Load())
	}
	if _, ok := server.rtspPullServer("live/expired"); ok {
		t.Fatal("expired create retained ownership")
	}
}

func TestRTSPPullControlShutdownDeletesSessions(t *testing.T) {
	var deletes atomic.Int32
	mediaServer := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		switch request.URL.Path {
		case "/rtsp/pull/create":
			writeJSON(writer, http.StatusCreated, map[string]string{"result": "ok"})
		case "/rtsp/pull/delete":
			deletes.Add(1)
			writeJSON(writer, http.StatusOK, map[string]string{"result": "ok"})
		default:
			http.NotFound(writer, request)
		}
	}))
	defer mediaServer.Close()

	registry := newMediaServerRegistry()
	if err := registry.register(mediaServerRegistration{
		ServerID: "media-1", InstanceID: "instance-a", ControlURL: mediaServer.URL, MediaIP: "127.0.0.1",
	}, time.Now()); err != nil {
		t.Fatalf("register() error = %v", err)
	}
	server := newInfrastructureServer(testConfig(), registry, slog.New(slog.NewTextHandler(io.Discard, nil)))
	request := httptest.NewRequest(http.MethodPost, "/internal/rtsp-pull/create", strings.NewReader(`{"stream_name":"live/shutdown","url":"rtsp://192.0.2.10/live"}`))
	request.Header.Set("Content-Type", "application/json")
	recorder := httptest.NewRecorder()
	server.handleRTSPPullCreate(recorder, request)
	if recorder.Code != http.StatusCreated {
		t.Fatalf("create response = %d %s", recorder.Code, recorder.Body.String())
	}

	server.shutdownRTSPPulls(context.Background())
	if deletes.Load() != 1 {
		t.Fatalf("shutdown deletes = %d", deletes.Load())
	}
	deleteRequest := httptest.NewRequest(http.MethodPost, "/internal/rtsp-pull/delete", strings.NewReader(`{"stream_name":"live/shutdown"}`))
	deleteRequest.Header.Set("Content-Type", "application/json")
	deleteResponse := httptest.NewRecorder()
	server.handleRTSPPullDelete(deleteResponse, deleteRequest)
	if deleteResponse.Code != http.StatusNotFound {
		t.Fatalf("delete after shutdown = %d %s", deleteResponse.Code, deleteResponse.Body.String())
	}
}

func TestRTSPPullControlMapsNetworkFailureWithoutLoggingPassword(t *testing.T) {
	mediaServer := httptest.NewServer(http.HandlerFunc(func(http.ResponseWriter, *http.Request) {}))
	controlURL := mediaServer.URL
	mediaServer.Close()

	registry := newMediaServerRegistry()
	if err := registry.register(mediaServerRegistration{
		ServerID: "media-1", InstanceID: "instance-a", ControlURL: controlURL, MediaIP: "127.0.0.1",
	}, time.Now()); err != nil {
		t.Fatalf("register() error = %v", err)
	}
	var logs bytes.Buffer
	server := newInfrastructureServer(testConfig(), registry, slog.New(slog.NewTextHandler(&logs, nil)))
	httpServer := httptest.NewServer(server.handler())
	defer httpServer.Close()
	username := "admin"
	password := "network-failure-password"
	response := postJSON(t, httpServer.Client(), httpServer.URL+"/internal/rtsp-pull/create", rtspPullCreateRequest{
		StreamName: "live/network-failure", URL: "rtsp://192.0.2.10/live", Username: &username, Password: &password,
	})
	assertHTTPError(t, response, http.StatusBadGateway, "rtsp_pull_create_failed")
	if strings.Contains(logs.String(), password) {
		t.Fatalf("password leaked in logs: %s", logs.String())
	}
}

func TestRTSPPullControlRemovesOwnershipWhenMediaSessionIsMissing(t *testing.T) {
	var deletes atomic.Int32
	mediaServer := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		switch request.URL.Path {
		case "/rtsp/pull/create":
			writeJSON(writer, http.StatusCreated, map[string]string{"result": "ok"})
		case "/rtsp/pull/delete":
			deletes.Add(1)
			writeHTTPError(writer, http.StatusNotFound, "not_found")
		default:
			http.NotFound(writer, request)
		}
	}))
	defer mediaServer.Close()

	registry := newMediaServerRegistry()
	if err := registry.register(mediaServerRegistration{
		ServerID: "media-1", InstanceID: "instance-a", ControlURL: mediaServer.URL, MediaIP: "127.0.0.1",
	}, time.Now()); err != nil {
		t.Fatalf("register() error = %v", err)
	}
	server := newInfrastructureServer(testConfig(), registry, slog.New(slog.NewTextHandler(io.Discard, nil)))
	httpServer := httptest.NewServer(server.handler())
	defer httpServer.Close()
	command := rtspPullCreateRequest{StreamName: "live/missing", URL: "rtsp://192.0.2.10/live"}
	response := postJSON(t, httpServer.Client(), httpServer.URL+"/internal/rtsp-pull/create", command)
	if response.StatusCode != http.StatusCreated {
		t.Fatalf("create status = %d body = %s", response.StatusCode, readBody(t, response))
	}
	response.Body.Close()

	for range 2 {
		response = postJSON(t, httpServer.Client(), httpServer.URL+"/internal/rtsp-pull/delete", map[string]string{"stream_name": command.StreamName})
		assertHTTPError(t, response, http.StatusNotFound, "not_found")
	}
	if deletes.Load() != 1 {
		t.Fatalf("media deletes = %d", deletes.Load())
	}
}
