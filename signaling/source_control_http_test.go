package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/google/uuid"
)

type sourceMediaCommand struct {
	path string
	body rtspPullCreateRequest

	streamID   string
	streamName string
}

func TestSourceControlStartsStopsAndRestartsRuntime(t *testing.T) {
	commands := make(chan sourceMediaCommand, 4)
	media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		switch request.URL.Path {
		case "/rtsp/pull/create":
			var command rtspPullCreateRequest
			if err := json.NewDecoder(request.Body).Decode(&command); err != nil {
				t.Errorf("decode create: %v", err)
			}
			commands <- sourceMediaCommand{path: request.URL.Path, body: command}
			writeJSON(writer, http.StatusCreated, map[string]string{"result": "ok"})
		case "/rtsp/pull/delete":
			var command struct {
				StreamID   string `json:"stream_id"`
				StreamName string `json:"stream_name"`
			}
			if err := json.NewDecoder(request.Body).Decode(&command); err != nil {
				t.Errorf("decode delete: %v", err)
			}
			commands <- sourceMediaCommand{path: request.URL.Path, streamID: command.StreamID, streamName: command.StreamName}
			writeJSON(writer, http.StatusOK, map[string]string{"result": "ok"})
		default:
			http.NotFound(writer, request)
		}
	}))
	defer media.Close()
	server := newSourceControlTestServer(t, media.URL)
	source := createControlTestSource(t, server, "live/camera", "admin", "secret")

	firstID := startControlTestSource(t, server, source.sourceID, http.StatusCreated)
	create := waitSourceControlRequest(t, commands)
	if create.path != "/rtsp/pull/create" || create.body.StreamID != firstID || create.body.SourceID == nil ||
		*create.body.SourceID != source.sourceID || create.body.StreamName != source.streamName || create.body.URL != source.url ||
		create.body.Username == nil || *create.body.Username != "admin" || create.body.Password == nil || *create.body.Password != "secret" {
		t.Fatalf("create command = %+v", create)
	}
	stored, err := server.sources.get(t.Context(), source.sourceID)
	if err != nil || stored.desiredState != sourceDesiredRunning {
		t.Fatalf("source after start = %+v, %v", stored, err)
	}

	startingEvent := observedRuntime{
		Type: "source_started", ServerID: "media-1", InstanceID: "instance-a", StreamID: firstID,
		StreamName: source.streamName, SourceID: source.sourceID, Direction: "input", Protocol: "rtsp",
		State: "starting", Stage: "resolving",
	}
	if _, err := server.runtimes.apply(startingEvent); err != nil {
		t.Fatalf("apply(starting) error = %v", err)
	}
	startControlTestSource(t, server, source.sourceID, http.StatusConflict)

	stopControlTestSource(t, server, source.sourceID, http.StatusOK)
	remove := waitSourceControlRequest(t, commands)
	if remove.path != "/rtsp/pull/delete" || remove.streamID != firstID || remove.streamName != source.streamName {
		t.Fatalf("delete command = %+v", remove)
	}
	stored, err = server.sources.get(t.Context(), source.sourceID)
	if err != nil || stored.desiredState != sourceDesiredStopped {
		t.Fatalf("source after stop = %+v, %v", stored, err)
	}
	stopControlTestSource(t, server, source.sourceID, http.StatusOK)

	startingEvent.Type = "source_stopped"
	startingEvent.State = "stopped"
	startingEvent.Stage = ""
	startingEvent.EndReason = "requested"
	if _, err := server.runtimes.apply(startingEvent); err != nil {
		t.Fatalf("apply(stopped) error = %v", err)
	}
	secondID := startControlTestSource(t, server, source.sourceID, http.StatusCreated)
	if secondID == firstID || !validUUIDv4(secondID) {
		t.Fatalf("restart stream ID = %q, first = %q", secondID, firstID)
	}
	if create = waitSourceControlRequest(t, commands); create.body.StreamID != secondID {
		t.Fatalf("restart command = %+v", create)
	}
}

func TestSourceControlPersistsDesiredStateAcrossCommandFailure(t *testing.T) {
	var requests int
	var streamIDs []string
	media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		var command rtspPullCreateRequest
		if err := json.NewDecoder(request.Body).Decode(&command); err != nil {
			t.Errorf("decode create: %v", err)
		}
		streamIDs = append(streamIDs, command.StreamID)
		requests++
		if requests == 1 {
			writeHTTPError(writer, http.StatusInternalServerError, "operation_failed")
			return
		}
		writeJSON(writer, http.StatusCreated, map[string]string{"result": "ok"})
	}))
	defer media.Close()
	server := newSourceControlTestServer(t, media.URL)
	source := createControlTestSource(t, server, "live/retry", "", "")

	startControlTestSource(t, server, source.sourceID, http.StatusBadGateway)
	stored, err := server.sources.get(t.Context(), source.sourceID)
	if err != nil || stored.desiredState != sourceDesiredRunning {
		t.Fatalf("source after failed start = %+v, %v", stored, err)
	}
	if _, ok := server.rtspSourceRuntime(source.sourceID); ok {
		t.Fatal("failed start retained runtime ownership")
	}
	secondID := startControlTestSource(t, server, source.sourceID, http.StatusCreated)
	if len(streamIDs) != 2 || streamIDs[0] == streamIDs[1] || secondID != streamIDs[1] {
		t.Fatalf("start stream IDs = %v response=%q", streamIDs, secondID)
	}
}

func TestSourceControlStopRecoversFromLostTerminalEvent(t *testing.T) {
	var creates []string
	media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		switch request.URL.Path {
		case "/rtsp/pull/create":
			var command rtspPullCreateRequest
			if err := json.NewDecoder(request.Body).Decode(&command); err != nil {
				t.Errorf("decode create: %v", err)
			}
			creates = append(creates, command.StreamID)
			writeJSON(writer, http.StatusCreated, map[string]string{"result": "ok"})
		case "/rtsp/pull/delete":
			writeHTTPError(writer, http.StatusNotFound, "not_found")
		default:
			http.NotFound(writer, request)
		}
	}))
	defer media.Close()
	server := newSourceControlTestServer(t, media.URL)
	source := createControlTestSource(t, server, "live/lost-terminal", "", "")

	firstID := startControlTestSource(t, server, source.sourceID, http.StatusCreated)
	active := observedRuntime{
		Type: "source_started", ServerID: "media-1", InstanceID: "instance-a", StreamID: firstID,
		StreamName: source.streamName, SourceID: source.sourceID, Direction: "input", Protocol: "rtsp",
		State: "streaming", Stage: "streaming",
	}
	if _, err := server.runtimes.apply(active); err != nil {
		t.Fatalf("apply(active) error = %v", err)
	}
	stopControlTestSource(t, server, source.sourceID, http.StatusOK)
	current, ok := server.runtimes.currentForSource(source.sourceID)
	if !ok || current.StreamID != firstID || current.Type != "source_stopped" || current.State != "stopped" ||
		current.EndReason != "requested" {
		t.Fatalf("observed after acknowledged stop = %+v, %v", current, ok)
	}

	secondID := startControlTestSource(t, server, source.sourceID, http.StatusCreated)
	if secondID == firstID || len(creates) != 2 || creates[0] != firstID || creates[1] != secondID {
		t.Fatalf("restart IDs = %q/%q, creates = %v", firstID, secondID, creates)
	}
}

func TestSourceControlSerializesConcurrentStarts(t *testing.T) {
	createStarted := make(chan struct{})
	releaseCreate := make(chan struct{})
	media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, _ *http.Request) {
		close(createStarted)
		<-releaseCreate
		writeJSON(writer, http.StatusCreated, map[string]string{"result": "ok"})
	}))
	defer media.Close()
	server := newSourceControlTestServer(t, media.URL)
	source := createControlTestSource(t, server, "live/concurrent", "", "")
	done := make(chan *httptest.ResponseRecorder, 1)
	go func() {
		done <- sourceRequest(t, server.handler(), http.MethodPost, "/api/sources/"+source.sourceID+"/start", "", "")
	}()
	select {
	case <-createStarted:
	case <-time.After(time.Second):
		t.Fatal("first create did not start")
	}
	second := sourceRequest(t, server.handler(), http.MethodPost, "/api/sources/"+source.sourceID+"/start", "", "")
	if second.Code != http.StatusConflict {
		t.Fatalf("concurrent start status/body = %d %s", second.Code, second.Body.String())
	}
	close(releaseCreate)
	if first := <-done; first.Code != http.StatusCreated {
		t.Fatalf("first start status/body = %d %s", first.Code, first.Body.String())
	}
}

func TestSourceControlStopsRuntimeCreatedAfterPendingStop(t *testing.T) {
	createStarted := make(chan struct{})
	releaseCreate := make(chan struct{})
	deleted := make(chan sourceMediaCommand, 1)
	media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		switch request.URL.Path {
		case "/rtsp/pull/create":
			close(createStarted)
			<-releaseCreate
			writeJSON(writer, http.StatusCreated, map[string]string{"result": "ok"})
		case "/rtsp/pull/delete":
			var command struct {
				StreamID   string `json:"stream_id"`
				StreamName string `json:"stream_name"`
			}
			if err := json.NewDecoder(request.Body).Decode(&command); err != nil {
				t.Errorf("decode delete: %v", err)
			}
			deleted <- sourceMediaCommand{
				path: request.URL.Path, streamID: command.StreamID, streamName: command.StreamName,
			}
			writeJSON(writer, http.StatusOK, map[string]string{"result": "ok"})
		default:
			http.NotFound(writer, request)
		}
	}))
	defer media.Close()
	server := newSourceControlTestServer(t, media.URL)
	source := createControlTestSource(t, server, "live/pending-stop", "", "")
	started := make(chan *httptest.ResponseRecorder, 1)
	go func() {
		started <- sourceRequest(t, server.handler(), http.MethodPost, "/api/sources/"+source.sourceID+"/start", "", "")
	}()
	select {
	case <-createStarted:
	case <-time.After(time.Second):
		t.Fatal("create did not start")
	}

	stopControlTestSource(t, server, source.sourceID, http.StatusConflict)
	stored, err := server.sources.get(t.Context(), source.sourceID)
	if err != nil || stored.desiredState != sourceDesiredStopped {
		t.Fatalf("source after pending stop = %+v, %v", stored, err)
	}
	close(releaseCreate)
	response := <-started
	if response.Code != http.StatusConflict {
		t.Fatalf("pending start status/body = %d %s", response.Code, response.Body.String())
	}
	remove := waitSourceControlRequest(t, deleted)
	if remove.path != "/rtsp/pull/delete" || remove.streamName != source.streamName || !validUUIDv4(remove.streamID) {
		t.Fatalf("delete command = %+v", remove)
	}
	if _, ok := server.rtspSourceRuntime(source.sourceID); ok {
		t.Fatal("pending stop retained runtime ownership")
	}
}

func TestSourceControlReconcilesTerminalBeforeCreateResponse(t *testing.T) {
	registry := newMediaServerRegistry()
	server := newTestInfrastructureServer(
		t, testConfig(), registry, slog.New(slog.NewTextHandler(io.Discard, nil)))
	control := httptest.NewServer(server.handler())
	defer control.Close()
	var streamIDs []string
	media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		var command rtspPullCreateRequest
		if err := json.NewDecoder(request.Body).Decode(&command); err != nil {
			t.Errorf("decode create: %v", err)
		}
		streamIDs = append(streamIDs, command.StreamID)
		for _, event := range []map[string]any{
			{"type": "source_started", "state": "starting", "stage": "resolving"},
			{"type": "runtime_error", "state": "stopped", "end_reason": "runtime_error", "error": "connect_failed"},
		} {
			event["server_id"] = "media-1"
			event["instance_id"] = "instance-a"
			event["stream_id"] = command.StreamID
			event["stream_name"] = command.StreamName
			event["source_id"] = *command.SourceID
			event["direction"] = "input"
			event["protocol"] = "rtsp"
			response := postJSON(t, control.Client(), control.URL+"/internal/runtime-events", event)
			if response.StatusCode != http.StatusNoContent {
				t.Errorf("event status/body = %d %s", response.StatusCode, readBody(t, response))
			}
			response.Body.Close()
		}
		writeJSON(writer, http.StatusCreated, map[string]string{"result": "ok"})
	}))
	defer media.Close()
	if err := registry.register(mediaServerRegistration{
		ServerID: "media-1", InstanceID: "instance-a", ControlURL: media.URL, MediaIP: "127.0.0.1",
	}, time.Now()); err != nil {
		t.Fatalf("register() error = %v", err)
	}
	source := createControlTestSource(t, server, "live/fast-failure", "", "")

	firstID := startControlTestSource(t, server, source.sourceID, http.StatusCreated)
	if _, ok := server.rtspSourceRuntime(source.sourceID); ok {
		t.Fatal("terminal-before-response retained dead runtime")
	}
	current, ok := server.runtimes.currentForSource(source.sourceID)
	if !ok || current.StreamID != firstID || current.State != "stopped" {
		t.Fatalf("observed after fast failure = %+v, %v", current, ok)
	}
	secondID := startControlTestSource(t, server, source.sourceID, http.StatusCreated)
	if secondID == firstID || len(streamIDs) != 2 {
		t.Fatalf("fast failure restart IDs = %q/%q commands=%v", firstID, secondID, streamIDs)
	}
}

func TestSourceControlDoesNotRestoreGenerationAfterTerminalCreateFailure(t *testing.T) {
	registry := newMediaServerRegistry()
	server := newTestInfrastructureServer(
		t, testConfig(), registry, slog.New(slog.NewTextHandler(io.Discard, nil)))
	control := httptest.NewServer(server.handler())
	defer control.Close()
	media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		var command rtspPullCreateRequest
		if err := json.NewDecoder(request.Body).Decode(&command); err != nil {
			t.Errorf("decode create: %v", err)
		}
		for _, event := range []map[string]any{
			{"type": "source_started", "state": "starting", "stage": "resolving"},
			{"type": "runtime_error", "state": "stopped", "end_reason": "runtime_error", "error": "startup_failed"},
		} {
			event["server_id"] = "media-1"
			event["instance_id"] = "instance-a"
			event["stream_id"] = command.StreamID
			event["stream_name"] = command.StreamName
			event["source_id"] = *command.SourceID
			event["direction"] = "input"
			event["protocol"] = "rtsp"
			response := postJSON(t, control.Client(), control.URL+"/internal/runtime-events", event)
			if response.StatusCode != http.StatusNoContent {
				t.Errorf("event status/body = %d %s", response.StatusCode, readBody(t, response))
			}
			response.Body.Close()
		}
		writeHTTPError(writer, http.StatusInternalServerError, "operation_failed")
	}))
	defer media.Close()
	if err := registry.register(mediaServerRegistration{
		ServerID: "media-1", InstanceID: "instance-a", ControlURL: media.URL, MediaIP: "127.0.0.1",
	}, time.Now()); err != nil {
		t.Fatalf("register() error = %v", err)
	}
	source := createControlTestSource(t, server, "live/failed-after-terminal", "", "")

	startControlTestSource(t, server, source.sourceID, http.StatusBadGateway)
	if _, ok := server.rtspSourceRuntime(source.sourceID); ok {
		t.Fatal("failed start restored terminated runtime ownership")
	}
	current, ok := server.runtimes.currentForSource(source.sourceID)
	if !ok || current.SourceID != source.sourceID || current.State != "stopped" || current.Error != "startup_failed" {
		t.Fatalf("observed after failed create = %+v, %v", current, ok)
	}
}

func TestSourceControlRejectsCreateCompletedAfterMediaServerOffline(t *testing.T) {
	createStarted := make(chan rtspPullCreateRequest, 1)
	releaseCreate := make(chan struct{})
	deleted := make(chan sourceMediaCommand, 1)
	media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		switch request.URL.Path {
		case "/rtsp/pull/create":
			var command rtspPullCreateRequest
			if err := json.NewDecoder(request.Body).Decode(&command); err != nil {
				t.Errorf("decode create: %v", err)
			}
			createStarted <- command
			<-releaseCreate
			writeJSON(writer, http.StatusCreated, map[string]string{"result": "ok"})
		case "/rtsp/pull/delete":
			var command struct {
				StreamID   string `json:"stream_id"`
				StreamName string `json:"stream_name"`
			}
			if err := json.NewDecoder(request.Body).Decode(&command); err != nil {
				t.Errorf("decode delete: %v", err)
			}
			deleted <- sourceMediaCommand{
				path: request.URL.Path, streamID: command.StreamID, streamName: command.StreamName,
			}
			writeJSON(writer, http.StatusOK, map[string]string{"result": "ok"})
		default:
			http.NotFound(writer, request)
		}
	}))
	defer media.Close()
	server := newSourceControlTestServer(t, media.URL)
	source := createControlTestSource(t, server, "live/offline", "", "")
	started := make(chan *httptest.ResponseRecorder, 1)
	go func() {
		started <- sourceRequest(t, server.handler(), http.MethodPost, "/api/sources/"+source.sourceID+"/start", "", "")
	}()
	var command rtspPullCreateRequest
	select {
	case command = <-createStarted:
	case <-time.After(time.Second):
		t.Fatal("create did not start")
	}
	offline := server.registry.expire(time.Now().Add(time.Hour), time.Minute)
	if len(offline) != 1 {
		t.Fatalf("expired media servers = %d", len(offline))
	}
	server.removeRTSPPullsForMediaServer(offline[0])
	server.runtimes.mediaServerOffline(offline[0].serverID, offline[0].instanceID)
	close(releaseCreate)
	response := <-started
	if response.Code != http.StatusServiceUnavailable {
		t.Fatalf("offline create status/body = %d %s", response.Code, response.Body.String())
	}
	remove := waitSourceControlRequest(t, deleted)
	if remove.streamID != command.StreamID || remove.streamName != source.streamName {
		t.Fatalf("offline cleanup command = %+v", remove)
	}
	if _, ok := server.rtspSourceRuntime(source.sourceID); ok {
		t.Fatal("offline create retained runtime ownership")
	}
}

func TestSourceControlRestoresBindingAfterOfflineCreateFailure(t *testing.T) {
	createStarted := make(chan struct{})
	releaseCreate := make(chan struct{})
	media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		if request.URL.Path != "/rtsp/pull/create" {
			http.NotFound(writer, request)
			return
		}
		close(createStarted)
		<-releaseCreate
		writeHTTPError(writer, http.StatusInternalServerError, "operation_failed")
	}))
	defer media.Close()
	server := newSourceControlTestServer(t, media.URL)
	source := createControlTestSource(t, server, "live/offline-failure", "", "")
	started := make(chan *httptest.ResponseRecorder, 1)
	go func() {
		started <- sourceRequest(t, server.handler(), http.MethodPost, "/api/sources/"+source.sourceID+"/start", "", "")
	}()
	select {
	case <-createStarted:
	case <-time.After(time.Second):
		t.Fatal("create did not start")
	}
	offline := server.registry.expire(time.Now().Add(time.Hour), time.Minute)
	if len(offline) != 1 {
		t.Fatalf("expired media servers = %d", len(offline))
	}
	server.removeRTSPPullsForMediaServer(offline[0])
	server.runtimes.mediaServerOffline(offline[0].serverID, offline[0].instanceID)
	close(releaseCreate)
	response := <-started
	if response.Code != http.StatusBadGateway {
		t.Fatalf("failed create status/body = %d %s", response.Code, response.Body.String())
	}
	if _, bound := server.runtimes.currentBySource[source.sourceID]; bound {
		t.Fatal("failed create retained unobserved source binding")
	}
}

func TestSourceControlCompensatesAmbiguousCreateFailure(t *testing.T) {
	created := make(chan rtspPullCreateRequest, 1)
	deleted := make(chan sourceMediaCommand, 1)
	media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		switch request.URL.Path {
		case "/rtsp/pull/create":
			var command rtspPullCreateRequest
			if err := json.NewDecoder(request.Body).Decode(&command); err != nil {
				t.Errorf("decode create: %v", err)
			}
			created <- command
			writer.Header().Set("Content-Type", "text/plain")
			writer.WriteHeader(http.StatusCreated)
		case "/rtsp/pull/delete":
			var command struct {
				StreamID   string `json:"stream_id"`
				StreamName string `json:"stream_name"`
			}
			if err := json.NewDecoder(request.Body).Decode(&command); err != nil {
				t.Errorf("decode delete: %v", err)
			}
			deleted <- sourceMediaCommand{
				path: request.URL.Path, streamID: command.StreamID, streamName: command.StreamName,
			}
			writeJSON(writer, http.StatusOK, map[string]string{"result": "ok"})
		default:
			http.NotFound(writer, request)
		}
	}))
	defer media.Close()
	server := newSourceControlTestServer(t, media.URL)
	source := createControlTestSource(t, server, "live/ambiguous", "", "")

	startControlTestSource(t, server, source.sourceID, http.StatusBadGateway)
	create := <-created
	remove := waitSourceControlRequest(t, deleted)
	if remove.streamID != create.StreamID || remove.streamName != create.StreamName {
		t.Fatalf("compensating delete = %+v, create = %+v", remove, create)
	}
	if _, ok := server.rtspSourceRuntime(source.sourceID); ok {
		t.Fatal("ambiguous create retained runtime ownership after compensation")
	}
}

func TestSourceControlShutdownWaitsForAdmittedCreate(t *testing.T) {
	createStarted := make(chan rtspPullCreateRequest, 1)
	releaseCreate := make(chan struct{})
	deleted := make(chan sourceMediaCommand, 1)
	media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		switch request.URL.Path {
		case "/rtsp/pull/create":
			var command rtspPullCreateRequest
			if err := json.NewDecoder(request.Body).Decode(&command); err != nil {
				t.Errorf("decode create: %v", err)
			}
			createStarted <- command
			<-releaseCreate
			writeJSON(writer, http.StatusCreated, map[string]string{"result": "ok"})
		case "/rtsp/pull/delete":
			var command struct {
				StreamID   string `json:"stream_id"`
				StreamName string `json:"stream_name"`
			}
			if err := json.NewDecoder(request.Body).Decode(&command); err != nil {
				t.Errorf("decode delete: %v", err)
			}
			deleted <- sourceMediaCommand{
				path: request.URL.Path, streamID: command.StreamID, streamName: command.StreamName,
			}
			writeJSON(writer, http.StatusOK, map[string]string{"result": "ok"})
		default:
			http.NotFound(writer, request)
		}
	}))
	defer media.Close()
	server := newSourceControlTestServer(t, media.URL)
	source := createControlTestSource(t, server, "live/shutdown", "", "")
	started := make(chan *httptest.ResponseRecorder, 1)
	go func() {
		started <- sourceRequest(t, server.handler(), http.MethodPost, "/api/sources/"+source.sourceID+"/start", "", "")
	}()
	var create rtspPullCreateRequest
	select {
	case create = <-createStarted:
	case <-time.After(time.Second):
		t.Fatal("create did not start")
	}
	shutdown := make(chan struct{})
	go func() {
		server.shutdownRTSPPulls(context.Background())
		close(shutdown)
	}()
	deadline := time.Now().Add(time.Second)
	for {
		server.sourceControlMu.Lock()
		closed := server.sourceControlClosed
		server.sourceControlMu.Unlock()
		if closed {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("source control did not close")
		}
		time.Sleep(time.Millisecond)
	}
	select {
	case remove := <-deleted:
		t.Fatalf("shutdown deleted before create completed: %+v", remove)
	default:
	}
	response := sourceRequest(t, server.handler(), http.MethodPost, "/api/sources/"+source.sourceID+"/stop", "", "")
	if response.Code != http.StatusServiceUnavailable {
		t.Fatalf("operation after shutdown status/body = %d %s", response.Code, response.Body.String())
	}
	close(releaseCreate)
	if response = <-started; response.Code != http.StatusCreated {
		t.Fatalf("admitted create status/body = %d %s", response.Code, response.Body.String())
	}
	remove := waitSourceControlRequest(t, deleted)
	if remove.streamID != create.StreamID || remove.streamName != create.StreamName {
		t.Fatalf("shutdown delete = %+v, create = %+v", remove, create)
	}
	select {
	case <-shutdown:
	case <-time.After(time.Second):
		t.Fatal("source control shutdown did not finish")
	}
}

func TestSourceControlUsesRuntimeNameAfterSourcePatch(t *testing.T) {
	deletes := make(chan string, 1)
	media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		if request.URL.Path == "/rtsp/pull/delete" {
			var command struct {
				StreamName string `json:"stream_name"`
			}
			_ = json.NewDecoder(request.Body).Decode(&command)
			deletes <- command.StreamName
			writeJSON(writer, http.StatusOK, map[string]string{"result": "ok"})
			return
		}
		writeJSON(writer, http.StatusCreated, map[string]string{"result": "ok"})
	}))
	defer media.Close()
	server := newSourceControlTestServer(t, media.URL)
	source := createControlTestSource(t, server, "live/original", "", "")
	startControlTestSource(t, server, source.sourceID, http.StatusCreated)
	response := sourceRequest(t, server.handler(), http.MethodPatch, "/api/sources/"+source.sourceID,
		`{"stream_name":"live/renamed"}`, "application/json")
	if response.Code != http.StatusOK {
		t.Fatalf("patch status/body = %d %s", response.Code, response.Body.String())
	}
	startControlTestSource(t, server, source.sourceID, http.StatusConflict)
	stopControlTestSource(t, server, source.sourceID, http.StatusOK)
	if name := <-deletes; name != "live/original" {
		t.Fatalf("delete stream name = %q", name)
	}
}

func TestSourceDeletePreservesSourceWhenRuntimeDeleteFails(t *testing.T) {
	var deletes int
	media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		if request.URL.Path == "/rtsp/pull/create" {
			writeJSON(writer, http.StatusCreated, map[string]string{"result": "ok"})
			return
		}
		deletes++
		if deletes == 1 {
			writeHTTPError(writer, http.StatusInternalServerError, "operation_failed")
			return
		}
		writeJSON(writer, http.StatusOK, map[string]string{"result": "ok"})
	}))
	defer media.Close()
	server := newSourceControlTestServer(t, media.URL)
	source := createControlTestSource(t, server, "live/delete", "", "")
	streamID := startControlTestSource(t, server, source.sourceID, http.StatusCreated)

	response := sourceRequest(t, server.handler(), http.MethodDelete, "/api/sources/"+source.sourceID, "", "")
	if response.Code != http.StatusBadGateway {
		t.Fatalf("failed delete status/body = %d %s", response.Code, response.Body.String())
	}
	stored, err := server.sources.get(t.Context(), source.sourceID)
	if err != nil || stored.desiredState != sourceDesiredStopped {
		t.Fatalf("source after failed delete = %+v, %v", stored, err)
	}
	if _, ok := server.rtspSourceRuntime(source.sourceID); !ok {
		t.Fatal("failed delete did not restore runtime ownership")
	}
	response = sourceRequest(t, server.handler(), http.MethodDelete, "/api/sources/"+source.sourceID, "", "")
	if response.Code != http.StatusOK {
		t.Fatalf("retry delete status/body = %d %s", response.Code, response.Body.String())
	}
	if _, err := server.sources.get(t.Context(), source.sourceID); !errors.Is(err, errSourceNotFound) {
		t.Fatalf("source after delete error = %v", err)
	}
	if _, bound := server.runtimes.currentBySource[source.sourceID]; bound {
		t.Fatal("deleted source retained its runtime binding")
	}
	if runtime, exists := server.runtimes.byStreamID[streamID]; !exists || runtime.State != "stopped" {
		t.Fatalf("deleted source runtime = %+v, %v", runtime, exists)
	}
}

func TestSourceDeleteUnbindsUnobservedRuntime(t *testing.T) {
	server := newSourceControlTestServer(t, "http://127.0.0.1:1")
	source := createControlTestSource(t, server, "live/delete-unobserved", "", "")
	streamID := uuid.NewString()
	server.runtimes.bindSource(source.sourceID, streamID)

	response := sourceRequest(t, server.handler(), http.MethodDelete, "/api/sources/"+source.sourceID, "", "")
	if response.Code != http.StatusOK {
		t.Fatalf("delete status/body = %d %s", response.Code, response.Body.String())
	}
	if _, bound := server.runtimes.currentBySource[source.sourceID]; bound {
		t.Fatal("deleted source retained unobserved runtime binding")
	}
}

func TestSourceStartCompensatesConcurrentDelete(t *testing.T) {
	commands := make(chan sourceMediaCommand, 2)
	media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		switch request.URL.Path {
		case "/rtsp/pull/create":
			var command rtspPullCreateRequest
			if err := json.NewDecoder(request.Body).Decode(&command); err != nil {
				t.Errorf("decode create: %v", err)
			}
			commands <- sourceMediaCommand{path: request.URL.Path, body: command}
			writeJSON(writer, http.StatusCreated, map[string]string{"result": "ok"})
		case "/rtsp/pull/delete":
			var command struct {
				StreamID   string `json:"stream_id"`
				StreamName string `json:"stream_name"`
			}
			if err := json.NewDecoder(request.Body).Decode(&command); err != nil {
				t.Errorf("decode delete: %v", err)
			}
			commands <- sourceMediaCommand{path: request.URL.Path, streamID: command.StreamID, streamName: command.StreamName}
			writeJSON(writer, http.StatusOK, map[string]string{"result": "ok"})
		default:
			http.NotFound(writer, request)
		}
	}))
	defer media.Close()
	server := newSourceControlTestServer(t, media.URL)
	source := createControlTestSource(t, server, "live/start-delete-race", "", "")

	server.registry.mu.Lock()
	started := make(chan *httptest.ResponseRecorder, 1)
	go func() {
		started <- sourceRequest(t, server.handler(), http.MethodPost, "/api/sources/"+source.sourceID+"/start", "", "")
	}()
	deadline := time.Now().Add(time.Second)
	for {
		stored, err := server.sources.get(t.Context(), source.sourceID)
		if err == nil && stored.desiredState == sourceDesiredRunning {
			break
		}
		if time.Now().After(deadline) {
			server.registry.mu.Unlock()
			t.Fatalf("start did not update desired state: %+v, %v", stored, err)
		}
		time.Sleep(time.Millisecond)
	}
	deleted := sourceRequest(t, server.handler(), http.MethodDelete, "/api/sources/"+source.sourceID, "", "")
	if deleted.Code != http.StatusOK {
		server.registry.mu.Unlock()
		t.Fatalf("delete status/body = %d %s", deleted.Code, deleted.Body.String())
	}
	server.registry.mu.Unlock()

	response := <-started
	if response.Code != http.StatusConflict {
		t.Fatalf("racing start status/body = %d %s", response.Code, response.Body.String())
	}
	create := waitSourceControlRequest(t, commands)
	remove := waitSourceControlRequest(t, commands)
	if create.path != "/rtsp/pull/create" || remove.path != "/rtsp/pull/delete" ||
		remove.streamID != create.body.StreamID || remove.streamName != source.streamName {
		t.Fatalf("compensation commands = %+v / %+v", create, remove)
	}
	if _, ok := server.rtspSourceRuntime(source.sourceID); ok {
		t.Fatal("concurrent delete retained runtime ownership")
	}
	if _, bound := server.runtimes.currentBySource[source.sourceID]; bound {
		t.Fatal("concurrent delete retained source binding")
	}
}

func TestSourceDeleteCannotRemoveConcurrentReplacement(t *testing.T) {
	creates := make(chan rtspPullCreateRequest, 2)
	deleteStarted := make(chan struct{})
	releaseDelete := make(chan struct{})
	media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		switch request.URL.Path {
		case "/rtsp/pull/create":
			var command rtspPullCreateRequest
			if err := json.NewDecoder(request.Body).Decode(&command); err != nil {
				t.Errorf("decode create: %v", err)
			}
			creates <- command
			writeJSON(writer, http.StatusCreated, map[string]string{"result": "ok"})
		case "/rtsp/pull/delete":
			close(deleteStarted)
			<-releaseDelete
			writeJSON(writer, http.StatusOK, map[string]string{"result": "ok"})
		default:
			http.NotFound(writer, request)
		}
	}))
	defer media.Close()
	server := newSourceControlTestServer(t, media.URL)
	source := createControlTestSource(t, server, "live/delete-race", "", "")
	firstID := startControlTestSource(t, server, source.sourceID, http.StatusCreated)
	<-creates
	firstEvent := observedRuntime{
		Type: "source_started", ServerID: "media-1", InstanceID: "instance-a", StreamID: firstID,
		StreamName: source.streamName, SourceID: source.sourceID, Direction: "input", Protocol: "rtsp",
		State: "starting", Stage: "resolving",
	}
	if _, err := server.runtimes.apply(firstEvent); err != nil {
		t.Fatalf("apply(starting) error = %v", err)
	}
	deleted := make(chan *httptest.ResponseRecorder, 1)
	go func() {
		deleted <- sourceRequest(t, server.handler(), http.MethodDelete, "/api/sources/"+source.sourceID, "", "")
	}()
	select {
	case <-deleteStarted:
	case <-time.After(time.Second):
		t.Fatal("delete did not start")
	}
	firstEvent.Type = "source_stopped"
	firstEvent.State = "stopped"
	firstEvent.Stage = ""
	firstEvent.EndReason = "requested"
	if _, err := server.runtimes.apply(firstEvent); err != nil {
		t.Fatalf("apply(stopped) error = %v", err)
	}
	secondID := startControlTestSource(t, server, source.sourceID, http.StatusCreated)
	<-creates
	close(releaseDelete)
	response := <-deleted
	if response.Code != http.StatusConflict {
		t.Fatalf("racing delete status/body = %d %s", response.Code, response.Body.String())
	}
	stored, err := server.sources.get(t.Context(), source.sourceID)
	if err != nil || stored.desiredState != sourceDesiredRunning {
		t.Fatalf("replacement source = %+v, %v", stored, err)
	}
	current, ok := server.rtspSourceRuntime(source.sourceID)
	if !ok || current.streamID != secondID || current.streamID == firstID {
		t.Fatalf("replacement runtime = %+v, %v", current, ok)
	}
}

func TestSourceControlDoesNotLogPassword(t *testing.T) {
	media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, _ *http.Request) {
		writeHTTPError(writer, http.StatusInternalServerError, "operation_failed")
	}))
	defer media.Close()
	var logs bytes.Buffer
	server := newSourceControlTestServerWithLogger(t, media.URL, slog.New(slog.NewTextHandler(&logs, nil)))
	source := createControlTestSource(t, server, "live/private", "admin", "sensitive-password")
	startControlTestSource(t, server, source.sourceID, http.StatusBadGateway)
	if strings.Contains(logs.String(), "sensitive-password") {
		t.Fatalf("logs leak password: %s", logs.String())
	}
}

func TestSourceControlRemovesLegacyRoutes(t *testing.T) {
	server := newSourceControlTestServer(t, "http://127.0.0.1:1")
	for _, path := range []string{"/internal/rtsp-pull/create", "/internal/rtsp-pull/delete"} {
		response := sourceRequest(t, server.handler(), http.MethodPost, path, `{}`, "application/json")
		if response.Code != http.StatusNotFound {
			t.Fatalf("legacy route %s status = %d", path, response.Code)
		}
	}
}

func newSourceControlTestServer(t *testing.T, controlURL string) *infrastructureServer {
	return newSourceControlTestServerWithLogger(t, controlURL, slog.New(slog.NewTextHandler(io.Discard, nil)))
}

func newSourceControlTestServerWithLogger(t *testing.T, controlURL string, logger *slog.Logger) *infrastructureServer {
	t.Helper()
	registry := newMediaServerRegistry()
	if err := registry.register(mediaServerRegistration{
		ServerID: "media-1", InstanceID: "instance-a", ControlURL: controlURL, MediaIP: "127.0.0.1",
	}, time.Now()); err != nil {
		t.Fatalf("register() error = %v", err)
	}
	return newTestInfrastructureServer(t, testConfig(), registry, logger)
}

func createControlTestSource(t *testing.T, server *infrastructureServer, streamName, username, password string) rtspSource {
	t.Helper()
	source := rtspSource{
		sourceID: uuid.NewString(), streamName: streamName, url: "rtsp://camera.example/live",
		username: username, password: password, desiredState: sourceDesiredStopped,
	}
	if err := server.sources.create(t.Context(), source); err != nil {
		t.Fatalf("create source error = %v", err)
	}
	return source
}

func startControlTestSource(t *testing.T, server *infrastructureServer, sourceID string, want int) string {
	t.Helper()
	response := sourceRequest(t, server.handler(), http.MethodPost, "/api/sources/"+sourceID+"/start", "", "")
	if response.Code != want {
		t.Fatalf("start status/body = %d %s, want %d", response.Code, response.Body.String(), want)
	}
	if want != http.StatusCreated {
		return ""
	}
	var body struct {
		StreamID string `json:"stream_id"`
	}
	if err := json.Unmarshal(response.Body.Bytes(), &body); err != nil || !validUUIDv4(body.StreamID) {
		t.Fatalf("start response = %s, %v", response.Body.String(), err)
	}
	return body.StreamID
}

func stopControlTestSource(t *testing.T, server *infrastructureServer, sourceID string, want int) {
	t.Helper()
	response := sourceRequest(t, server.handler(), http.MethodPost, "/api/sources/"+sourceID+"/stop", "", "")
	if response.Code != want {
		t.Fatalf("stop status/body = %d %s, want %d", response.Code, response.Body.String(), want)
	}
}

func waitSourceControlRequest(t *testing.T, requests <-chan sourceMediaCommand) sourceMediaCommand {
	t.Helper()
	select {
	case request := <-requests:
		return request
	case <-time.After(time.Second):
		t.Fatal("media server request timeout")
		return sourceMediaCommand{}
	}
}
