package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
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

type sourceControlRoundTripFunc func(*http.Request) (*http.Response, error)

func (f sourceControlRoundTripFunc) RoundTrip(request *http.Request) (*http.Response, error) {
	return f(request)
}

type cancelAtEOFBody struct {
	reader io.Reader
	cancel context.CancelFunc
}

func (b *cancelAtEOFBody) Read(buffer []byte) (int, error) {
	read, err := b.reader.Read(buffer)
	if err == io.EOF {
		b.cancel()
	}
	return read, err
}

func (*cancelAtEOFBody) Close() error { return nil }

type sourceMediaResponse struct {
	status  int
	release <-chan struct{}
}

type sourceMediaScript struct {
	server          *httptest.Server
	creates         chan rtspPullCreateRequest
	deletes         chan sourceMediaCommand
	createResponses chan sourceMediaResponse
	deleteResponses chan sourceMediaResponse
}

func newSourceMediaScript(t *testing.T) *sourceMediaScript {
	t.Helper()
	script := &sourceMediaScript{
		creates: make(chan rtspPullCreateRequest, 8), deletes: make(chan sourceMediaCommand, 8),
		createResponses: make(chan sourceMediaResponse, 8), deleteResponses: make(chan sourceMediaResponse, 8),
	}
	script.server = httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		var response sourceMediaResponse
		switch request.URL.Path {
		case "/rtsp/pull/create":
			var command rtspPullCreateRequest
			if err := json.NewDecoder(request.Body).Decode(&command); err != nil {
				t.Errorf("decode create: %v", err)
			}
			script.creates <- command
			select {
			case response = <-script.createResponses:
			case <-request.Context().Done():
				return
			}
		case "/rtsp/pull/delete":
			var command struct {
				StreamID   string `json:"stream_id"`
				StreamName string `json:"stream_name"`
			}
			if err := json.NewDecoder(request.Body).Decode(&command); err != nil {
				t.Errorf("decode delete: %v", err)
			}
			script.deletes <- sourceMediaCommand{
				path: request.URL.Path, streamID: command.StreamID, streamName: command.StreamName,
			}
			select {
			case response = <-script.deleteResponses:
			case <-request.Context().Done():
				return
			}
		default:
			http.NotFound(writer, request)
			return
		}
		if response.release != nil {
			<-response.release
		}
		if response.status == 0 {
			panic(http.ErrAbortHandler)
		}
		if (request.URL.Path == "/rtsp/pull/create" && response.status == http.StatusCreated) ||
			(request.URL.Path == "/rtsp/pull/delete" && response.status == http.StatusOK) {
			writeJSON(writer, response.status, map[string]string{"result": "ok"})
			return
		}
		code := "operation_failed"
		if response.status == http.StatusNotFound {
			code = "not_found"
		}
		writeHTTPError(writer, response.status, code)
	}))
	t.Cleanup(script.server.Close)
	return script
}

func retainUnresolvedSourceControlPull(
	t *testing.T,
	script *sourceMediaScript,
	server *infrastructureServer,
	source rtspSource,
) rtspPullRuntime {
	t.Helper()
	script.createResponses <- sourceMediaResponse{}
	script.deleteResponses <- sourceMediaResponse{}
	startControlTestSource(t, server, source.sourceID, http.StatusBadGateway)
	create := waitSourceCreateRequest(t, script.creates)
	remove := waitSourceControlRequest(t, script.deletes)
	if remove.streamID != create.StreamID || remove.streamName != create.StreamName {
		t.Fatalf("compensating delete = %+v, create = %+v", remove, create)
	}
	runtime, ok := server.rtspSourceRuntime(source.sourceID)
	if !ok || runtime.streamID != create.StreamID || runtime.starting || runtime.createConfirmed || runtime.stopDone != nil {
		t.Fatalf("unresolved runtime = %+v, %v", runtime, ok)
	}
	if streamID, bound := server.runtimes.sourceBinding(source.sourceID); !bound || streamID != runtime.streamID {
		t.Fatalf("unresolved source binding = %q, %v", streamID, bound)
	}
	return runtime
}

func postSourceControlRuntimeEvent(
	t *testing.T,
	server *infrastructureServer,
	event observedRuntime,
	want int,
) {
	t.Helper()
	body, err := json.Marshal(event)
	if err != nil {
		t.Fatalf("marshal runtime event: %v", err)
	}
	response := sourceRequest(t, server.handler(), http.MethodPost, "/internal/runtime-events", string(body), "application/json")
	if response.Code != want {
		t.Fatalf("runtime event status/body = %d %s, want %d", response.Code, response.Body.String(), want)
	}
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

func TestSourceStopAcknowledgesRuntimeBeforeReleasingOwnership(t *testing.T) {
	media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		if request.URL.Path == "/rtsp/pull/create" {
			writeJSON(writer, http.StatusCreated, map[string]string{"result": "ok"})
			return
		}
		writeJSON(writer, http.StatusOK, map[string]string{"result": "ok"})
	}))
	defer media.Close()
	server := newSourceControlTestServer(t, media.URL)
	source := createControlTestSource(t, server, "live/stop-order", "", "")
	streamID := startControlTestSource(t, server, source.sourceID, http.StatusCreated)
	ownedDuringStopped := make(chan bool, 1)
	server.runtimes.setOnChange(func(event observedRuntime) {
		if event.StreamID != streamID || event.State != "stopped" {
			return
		}
		runtime, ok := server.rtspSourceRuntime(source.sourceID)
		ownedDuringStopped <- ok && runtime.streamID == streamID
	})

	stopControlTestSource(t, server, source.sourceID, http.StatusOK)
	if owned := <-ownedDuringStopped; !owned {
		t.Fatal("runtime ownership was released before stopped acknowledgment")
	}
	if _, ok := server.rtspSourceRuntime(source.sourceID); ok {
		t.Fatal("stopped runtime ownership remains")
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
	var server *infrastructureServer
	media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		switch request.URL.Path {
		case "/rtsp/pull/create":
			var command rtspPullCreateRequest
			if err := json.NewDecoder(request.Body).Decode(&command); err != nil {
				t.Errorf("decode create: %v", err)
			}
			created <- command
			if _, err := server.runtimes.apply(observedRuntime{
				Type: "source_started", ServerID: "media-1", InstanceID: "instance-a",
				StreamID: command.StreamID, StreamName: command.StreamName, SourceID: *command.SourceID,
				Direction: "input", Protocol: "rtsp", State: "starting", Stage: "resolving",
			}); err != nil {
				t.Errorf("apply starting runtime error = %v", err)
			}
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
	server = newSourceControlTestServer(t, media.URL)
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
	observed, ok := server.runtimes.currentForSource(source.sourceID)
	if !ok || observed.StreamID != create.StreamID || observed.State != "stopped" || observed.EndReason != "requested" {
		t.Fatalf("compensated runtime state = %+v, %v", observed, ok)
	}
}

func TestSourceControlRejectsReplacementWhileCreateIsUnresolved(t *testing.T) {
	script := newSourceMediaScript(t)
	server := newSourceControlTestServer(t, script.server.URL)
	source := createControlTestSource(t, server, "live/ambiguous-failed-cleanup", "", "")
	runtime := retainUnresolvedSourceControlPull(t, script, server, source)

	script.createResponses <- sourceMediaResponse{status: http.StatusCreated}
	startControlTestSource(t, server, source.sourceID, http.StatusConflict)
	select {
	case create := <-script.creates:
		t.Fatalf("unresolved replacement sent create %+v", create)
	default:
	}
	owned, ok := server.rtspSourceRuntime(source.sourceID)
	if !ok || !sameRTSPPull(owned, runtime) || owned.createConfirmed {
		t.Fatalf("unresolved ownership changed = %+v, %v", owned, ok)
	}
	if streamID, bound := server.runtimes.sourceBinding(source.sourceID); !bound || streamID != runtime.streamID {
		t.Fatalf("unresolved source binding = %q, %v", streamID, bound)
	}
	stored, err := server.sources.get(t.Context(), source.sourceID)
	if err != nil || stored.desiredState != sourceDesiredRunning {
		t.Fatalf("source after rejected replacement = %+v, %v", stored, err)
	}
}

func TestSourceControlConfirmsAmbiguousCreateCleanupOnNotFound(t *testing.T) {
	script := newSourceMediaScript(t)
	server := newSourceControlTestServer(t, script.server.URL)
	source := createControlTestSource(t, server, "live/ambiguous-not-found", "", "")
	script.createResponses <- sourceMediaResponse{}
	script.deleteResponses <- sourceMediaResponse{status: http.StatusNotFound}
	startControlTestSource(t, server, source.sourceID, http.StatusBadGateway)
	firstCreate := waitSourceCreateRequest(t, script.creates)
	remove := waitSourceControlRequest(t, script.deletes)
	if remove.streamID != firstCreate.StreamID || remove.streamName != firstCreate.StreamName {
		t.Fatalf("compensating delete = %+v, create = %+v", remove, firstCreate)
	}
	if _, ok := server.rtspSourceRuntime(source.sourceID); ok {
		t.Fatal("not-found compensation retained runtime ownership")
	}
	observed, ok := server.runtimes.currentForSource(source.sourceID)
	if !ok || observed.StreamID != firstCreate.StreamID || observed.State != "stopped" || observed.EndReason != "requested" {
		t.Fatalf("not-found compensation observed runtime = %+v, %v", observed, ok)
	}

	script.createResponses <- sourceMediaResponse{status: http.StatusCreated}
	secondID := startControlTestSource(t, server, source.sourceID, http.StatusCreated)
	secondCreate := waitSourceCreateRequest(t, script.creates)
	if secondID == firstCreate.StreamID || secondCreate.StreamID != secondID {
		t.Fatalf("replacement stream IDs = %q/%q, create = %+v", firstCreate.StreamID, secondID, secondCreate)
	}
}

func TestSourceControlStopsUnresolvedPull(t *testing.T) {
	tests := []struct {
		name       string
		response   sourceMediaResponse
		wantStatus int
		resolved   bool
	}{
		{name: "success", response: sourceMediaResponse{status: http.StatusOK}, wantStatus: http.StatusOK, resolved: true},
		{name: "not found", response: sourceMediaResponse{status: http.StatusNotFound}, wantStatus: http.StatusOK, resolved: true},
		{name: "network failure", response: sourceMediaResponse{}, wantStatus: http.StatusBadGateway},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			script := newSourceMediaScript(t)
			server := newSourceControlTestServer(t, script.server.URL)
			source := createControlTestSource(t, server, "live/unresolved-stop", "", "")
			runtime := retainUnresolvedSourceControlPull(t, script, server, source)

			script.deleteResponses <- test.response
			stopControlTestSource(t, server, source.sourceID, test.wantStatus)
			remove := waitSourceControlRequest(t, script.deletes)
			if remove.streamID != runtime.streamID || remove.streamName != runtime.streamName {
				t.Fatalf("stop delete = %+v, runtime = %+v", remove, runtime)
			}
			if test.resolved {
				if _, ok := server.rtspSourceRuntime(source.sourceID); ok {
					t.Fatal("confirmed cleanup retained runtime ownership")
				}
				script.createResponses <- sourceMediaResponse{status: http.StatusCreated}
				secondID := startControlTestSource(t, server, source.sourceID, http.StatusCreated)
				secondCreate := waitSourceCreateRequest(t, script.creates)
				if secondID == runtime.streamID || secondCreate.StreamID != secondID {
					t.Fatalf("restart stream IDs = %q/%q, create = %+v", runtime.streamID, secondID, secondCreate)
				}
				return
			}
			owned, ok := server.rtspSourceRuntime(source.sourceID)
			if !ok || !sameRTSPPull(owned, runtime) || owned.stopDone != nil || owned.createConfirmed {
				t.Fatalf("failed cleanup ownership = %+v, %v", owned, ok)
			}
			script.createResponses <- sourceMediaResponse{status: http.StatusCreated}
			startControlTestSource(t, server, source.sourceID, http.StatusConflict)
			select {
			case create := <-script.creates:
				t.Fatalf("failed cleanup allowed replacement %+v", create)
			default:
			}
		})
	}
}

func TestSourceControlLateEventsResolveUnresolvedPull(t *testing.T) {
	t.Run("started confirms owner", func(t *testing.T) {
		script := newSourceMediaScript(t)
		server := newSourceControlTestServer(t, script.server.URL)
		source := createControlTestSource(t, server, "live/unresolved-started", "", "")
		runtime := retainUnresolvedSourceControlPull(t, script, server, source)
		event := observedRuntime{
			Type: "source_started", ServerID: runtime.server.serverID, InstanceID: runtime.server.instanceID,
			StreamID: runtime.streamID, StreamName: runtime.streamName, SourceID: runtime.sourceID,
			Direction: "input", Protocol: "rtsp", State: "starting", Stage: "resolving",
		}
		mismatched := runtime
		mismatched.streamID = uuid.NewString()
		if server.confirmRTSPPull(mismatched) {
			t.Fatal("mismatched started event confirmed unresolved owner")
		}
		if owned, ok := server.rtspSourceRuntime(source.sourceID); !ok || owned.createConfirmed {
			t.Fatalf("runtime after mismatched confirmation = %+v, %v", owned, ok)
		}
		postSourceControlRuntimeEvent(t, server, event, http.StatusNoContent)
		owned, ok := server.rtspSourceRuntime(source.sourceID)
		if !ok || owned.sourceID != runtime.sourceID || owned.streamID != runtime.streamID ||
			owned.streamName != runtime.streamName || owned.server.serverID != runtime.server.serverID ||
			owned.server.instanceID != runtime.server.instanceID || !owned.createConfirmed {
			t.Fatalf("confirmed runtime = %+v, %v", owned, ok)
		}
		startControlTestSource(t, server, source.sourceID, http.StatusConflict)
		select {
		case create := <-script.creates:
			t.Fatalf("confirmed runtime allowed replacement %+v", create)
		default:
		}
	})

	t.Run("stopped releases exact owner", func(t *testing.T) {
		script := newSourceMediaScript(t)
		server := newSourceControlTestServer(t, script.server.URL)
		source := createControlTestSource(t, server, "live/unresolved-stopped", "", "")
		runtime := retainUnresolvedSourceControlPull(t, script, server, source)
		stopped := observedRuntime{
			Type: "source_stopped", ServerID: runtime.server.serverID, InstanceID: runtime.server.instanceID,
			StreamID: runtime.streamID, StreamName: runtime.streamName, SourceID: runtime.sourceID,
			Direction: "input", Protocol: "rtsp", State: "stopped", EndReason: "remote",
		}
		postSourceControlRuntimeEvent(t, server, stopped, http.StatusNoContent)
		if _, ok := server.rtspSourceRuntime(source.sourceID); ok {
			t.Fatal("late stopped event retained runtime ownership")
		}

		script.createResponses <- sourceMediaResponse{status: http.StatusCreated}
		secondID := startControlTestSource(t, server, source.sourceID, http.StatusCreated)
		secondCreate := waitSourceCreateRequest(t, script.creates)
		if secondID == runtime.streamID || secondCreate.StreamID != secondID {
			t.Fatalf("replacement stream IDs = %q/%q, create = %+v", runtime.streamID, secondID, secondCreate)
		}
		postSourceControlRuntimeEvent(t, server, stopped, http.StatusNoContent)
		owned, ok := server.rtspSourceRuntime(source.sourceID)
		if !ok || owned.streamID != secondID {
			t.Fatalf("late stopped event removed replacement = %+v, %v", owned, ok)
		}
		if streamID, bound := server.runtimes.sourceBinding(source.sourceID); !bound || streamID != secondID {
			t.Fatalf("replacement source binding = %q, %v", streamID, bound)
		}
	})
}

func TestSourceControlConcurrentStopsShareUnresolvedCleanup(t *testing.T) {
	script := newSourceMediaScript(t)
	server := newSourceControlTestServer(t, script.server.URL)
	source := createControlTestSource(t, server, "live/unresolved-concurrent-stop", "", "")
	runtime := retainUnresolvedSourceControlPull(t, script, server, source)
	releaseDelete := make(chan struct{})
	released := false
	defer func() {
		if !released {
			close(releaseDelete)
		}
	}()
	script.deleteResponses <- sourceMediaResponse{status: http.StatusOK, release: releaseDelete}
	responses := make(chan *httptest.ResponseRecorder, 2)
	for range 2 {
		go func() {
			responses <- sourceRequest(t, server.handler(), http.MethodPost,
				"/api/sources/"+source.sourceID+"/stop", "", "")
		}()
	}
	remove := waitSourceControlRequest(t, script.deletes)
	if remove.streamID != runtime.streamID || remove.streamName != runtime.streamName {
		t.Fatalf("shared stop delete = %+v, runtime = %+v", remove, runtime)
	}

	script.createResponses <- sourceMediaResponse{status: http.StatusCreated}
	startControlTestSource(t, server, source.sourceID, http.StatusConflict)
	select {
	case create := <-script.creates:
		t.Fatalf("pending stop allowed replacement %+v", create)
	default:
	}
	close(releaseDelete)
	released = true
	for range 2 {
		select {
		case response := <-responses:
			if response.Code != http.StatusOK {
				t.Fatalf("concurrent stop status/body = %d %s", response.Code, response.Body.String())
			}
		case <-time.After(time.Second):
			t.Fatal("concurrent stop did not finish")
		}
	}
	select {
	case extra := <-script.deletes:
		t.Fatalf("concurrent stop sent extra delete %+v", extra)
	default:
	}
	if _, ok := server.rtspSourceRuntime(source.sourceID); ok {
		t.Fatal("successful shared cleanup retained runtime ownership")
	}
}

func TestRTSPPullReservationRejectsOfflineMediaServer(t *testing.T) {
	server := newSourceControlTestServer(t, "http://127.0.0.1:1")
	instance, ok := server.registry.selectOnline()
	if !ok {
		t.Fatal("missing online media server")
	}
	if offline := server.registry.expire(time.Now().Add(time.Hour), time.Minute); len(offline) != 1 {
		t.Fatalf("expired media servers = %+v", offline)
	}
	runtime, _, _, err := server.reserveRTSPPull(rtspPullRuntime{
		sourceID: uuid.NewString(), streamName: "live/offline-reservation", server: instance, starting: true,
	})
	if !errors.Is(err, errMediaServerStale) {
		t.Fatalf("reserve error = %v", err)
	}
	if runtime.streamID != "" {
		t.Fatalf("offline reservation allocated stream ID %q", runtime.streamID)
	}
	if len(server.rtspPulls) != 0 {
		t.Fatalf("offline reservation retained %d owners", len(server.rtspPulls))
	}
}

func TestSourceControlRecreatesUnresolvedPullAfterInstanceOffline(t *testing.T) {
	script := newSourceMediaScript(t)
	server := newSourceControlTestServer(t, script.server.URL)
	source := createControlTestSource(t, server, "live/unresolved-offline", "", "")
	runtime := retainUnresolvedSourceControlPull(t, script, server, source)
	offline := server.registry.expire(time.Now().Add(time.Hour), time.Minute)
	if len(offline) != 1 || offline[0].instanceID != runtime.server.instanceID {
		t.Fatalf("expired media servers = %+v", offline)
	}
	server.removeRTSPPullsForMediaServer(offline[0])
	server.runtimes.mediaServerOffline(offline[0].serverID, offline[0].instanceID)
	if _, ok := server.rtspSourceRuntime(source.sourceID); ok {
		t.Fatal("offline instance retained unresolved ownership")
	}
	replacement := mediaServerRegistration{
		ServerID: "media-1", InstanceID: "instance-b", ControlURL: script.server.URL, MediaIP: "127.0.0.1",
		RTMPPort: 1935, RTSPPort: 8554, HTTPPort: 8080,
	}
	if err := server.registry.register(replacement, time.Now()); err != nil {
		t.Fatalf("register replacement media server: %v", err)
	}
	script.createResponses <- sourceMediaResponse{status: http.StatusCreated}
	secondID := startControlTestSource(t, server, source.sourceID, http.StatusCreated)
	secondCreate := waitSourceCreateRequest(t, script.creates)
	if secondID == runtime.streamID || secondCreate.StreamID != secondID {
		t.Fatalf("replacement stream IDs = %q/%q, create = %+v", runtime.streamID, secondID, secondCreate)
	}
	late := observedRuntime{
		Type: "source_stopped", ServerID: runtime.server.serverID, InstanceID: runtime.server.instanceID,
		StreamID: runtime.streamID, StreamName: runtime.streamName, SourceID: runtime.sourceID,
		Direction: "input", Protocol: "rtsp", State: "stopped", EndReason: "remote",
	}
	postSourceControlRuntimeEvent(t, server, late, http.StatusGone)
	owned, ok := server.rtspSourceRuntime(source.sourceID)
	if !ok || owned.streamID != secondID || owned.server.instanceID != replacement.InstanceID {
		t.Fatalf("replacement runtime after stale event = %+v, %v", owned, ok)
	}
}

func TestSourceControlRetainsOwnershipWhenPostCreateReadIsCanceled(t *testing.T) {
	server := newSourceControlTestServer(t, "http://media.example")
	source := createControlTestSource(t, server, "live/canceled-read", "", "")
	requestContext, cancel := context.WithCancel(t.Context())
	defer cancel()
	deleteCalls := 0
	server.media.client.Transport = sourceControlRoundTripFunc(func(request *http.Request) (*http.Response, error) {
		header := make(http.Header)
		header.Set("Content-Type", "application/json")
		body := io.ReadCloser(io.NopCloser(strings.NewReader(`{"result":"ok"}`)))
		status := http.StatusCreated
		if request.URL.Path == "/rtsp/pull/create" {
			body = &cancelAtEOFBody{reader: strings.NewReader(`{"result":"ok"}`), cancel: cancel}
		} else {
			deleteCalls++
			status = http.StatusOK
		}
		return &http.Response{StatusCode: status, Header: header, Body: body, Request: request}, nil
	})
	request := httptest.NewRequestWithContext(
		requestContext, http.MethodPost, "/api/sources/"+source.sourceID+"/start", nil)
	response := httptest.NewRecorder()
	server.handler().ServeHTTP(response, request)
	if response.Code != http.StatusBadGateway {
		t.Fatalf("start status/body = %d %s", response.Code, response.Body.String())
	}
	owned, ok := server.rtspSourceRuntime(source.sourceID)
	if !ok || owned.starting {
		t.Fatalf("retained runtime ownership = %+v, %v", owned, ok)
	}
	if streamID, bound := server.runtimes.sourceBinding(source.sourceID); !bound || streamID != owned.streamID {
		t.Fatalf("retained source binding = %q, %v", streamID, bound)
	}
	if deleteCalls != 0 {
		t.Fatalf("post-create read error triggered %d cleanup requests", deleteCalls)
	}

	stopControlTestSource(t, server, source.sourceID, http.StatusOK)
	if deleteCalls != 1 {
		t.Fatalf("stop delete calls = %d", deleteCalls)
	}
	if _, ok := server.rtspSourceRuntime(source.sourceID); ok {
		t.Fatal("stop retained runtime ownership")
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

func TestSourceDeleteChurnKeepsRuntimeRegistriesBounded(t *testing.T) {
	server := newSourceControlTestServer(t, "http://127.0.0.1:1")
	for index := range 1001 {
		source := rtspSource{
			sourceID:   fmt.Sprintf("10000000-0000-4000-8000-%012d", index),
			streamName: fmt.Sprintf("live/churn-%d", index), url: "rtsp://camera.example/live",
			desiredState: sourceDesiredStopped,
		}
		if err := server.sources.create(t.Context(), source); err != nil {
			t.Fatalf("create source %d error = %v", index, err)
		}
		runtime := observedRuntime{
			Type: "source_stopped", ServerID: "media-1", InstanceID: "instance-a",
			StreamID:   fmt.Sprintf("20000000-0000-4000-8000-%012d", index),
			StreamName: source.streamName, SourceID: source.sourceID, Direction: "input", Protocol: "rtsp",
			State: "stopped", EndReason: "requested",
		}
		server.runtimes.bindSource(source.sourceID, runtime.StreamID)
		if _, err := server.runtimes.apply(runtime); err != nil {
			t.Fatalf("apply runtime %d error = %v", index, err)
		}
		response := sourceRequest(t, server.handler(), http.MethodDelete, "/api/sources/"+source.sourceID, "", "")
		if response.Code != http.StatusOK {
			t.Fatalf("delete source %d status/body = %d %s", index, response.Code, response.Body.String())
		}
	}
	sources, err := server.sources.list(t.Context())
	if err != nil || len(sources) != 0 {
		t.Fatalf("sources after churn = %d, %v", len(sources), err)
	}
	if len(server.runtimes.currentBySource) != 0 {
		t.Fatalf("source bindings after churn = %d", len(server.runtimes.currentBySource))
	}
	if len(server.runtimes.recentStopped) != maxRecentStoppedRuntimes ||
		len(server.runtimes.byStreamID) != maxRecentStoppedRuntimes {
		t.Fatalf("runtime registry sizes after churn = %d/%d",
			len(server.runtimes.recentStopped), len(server.runtimes.byStreamID))
	}
	newestStreamID := fmt.Sprintf("20000000-0000-4000-8000-%012d", 1000)
	if _, exists := server.runtimes.byStreamID[newestStreamID]; !exists {
		t.Fatal("latest stopped source runtime was not retained")
	}
}

func TestSourceStartReservesBeforeConcurrentDelete(t *testing.T) {
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
			deleted <- sourceMediaCommand{path: request.URL.Path, streamID: command.StreamID, streamName: command.StreamName}
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
	deleteEntered := make(chan struct{})
	deleteResult := make(chan *httptest.ResponseRecorder, 1)
	go func() {
		close(deleteEntered)
		deleteResult <- sourceRequest(t, server.handler(), http.MethodDelete, "/api/sources/"+source.sourceID, "", "")
	}()
	<-deleteEntered
	select {
	case early := <-deleteResult:
		server.registry.mu.Unlock()
		close(releaseCreate)
		<-started
		t.Fatalf("delete completed before start reservation: %d %s", early.Code, early.Body.String())
	case <-time.After(50 * time.Millisecond):
	}
	server.registry.mu.Unlock()

	var create rtspPullCreateRequest
	select {
	case create = <-createStarted:
	case <-time.After(time.Second):
		t.Fatal("create did not start")
	}
	deleteResponse := <-deleteResult
	if deleteResponse.Code != http.StatusConflict {
		t.Fatalf("racing delete status/body = %d %s", deleteResponse.Code, deleteResponse.Body.String())
	}
	close(releaseCreate)
	startResponse := <-started
	if startResponse.Code != http.StatusConflict {
		t.Fatalf("racing start status/body = %d %s", startResponse.Code, startResponse.Body.String())
	}
	remove := waitSourceControlRequest(t, deleted)
	if remove.streamID != create.StreamID || remove.streamName != source.streamName {
		t.Fatalf("stopped runtime = %+v, create = %+v", remove, create)
	}
	if _, ok := server.rtspSourceRuntime(source.sourceID); ok {
		t.Fatal("concurrent delete retained runtime ownership")
	}
	stored, err := server.sources.get(t.Context(), source.sourceID)
	if err != nil || stored.desiredState != sourceDesiredStopped {
		t.Fatalf("source after racing controls = %+v, %v", stored, err)
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
	if !server.removeRTSPPull(rtspPullRuntime{
		sourceID: source.sourceID, streamID: firstID, streamName: source.streamName,
		server: mediaServerInstance{serverID: "media-1", instanceID: "instance-a"},
	}) {
		t.Fatal("stopped event did not release runtime ownership")
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

func TestSourceDeleteWaitsForConcurrentStopCleanup(t *testing.T) {
	deleteStarted := make(chan struct{})
	releaseDelete := make(chan struct{})
	deletes := make(chan sourceMediaCommand, 2)
	var deleteCalls atomic.Int32
	media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		switch request.URL.Path {
		case "/rtsp/pull/create":
			writeJSON(writer, http.StatusCreated, map[string]string{"result": "ok"})
		case "/rtsp/pull/delete":
			var command struct {
				StreamID   string `json:"stream_id"`
				StreamName string `json:"stream_name"`
			}
			if err := json.NewDecoder(request.Body).Decode(&command); err != nil {
				t.Errorf("decode delete: %v", err)
			}
			deletes <- sourceMediaCommand{path: request.URL.Path, streamID: command.StreamID, streamName: command.StreamName}
			if deleteCalls.Add(1) == 1 {
				close(deleteStarted)
				<-releaseDelete
				writeHTTPError(writer, http.StatusServiceUnavailable, "operation_failed")
				return
			}
			writeJSON(writer, http.StatusOK, map[string]string{"result": "ok"})
		default:
			http.NotFound(writer, request)
		}
	}))
	defer media.Close()
	server := newSourceControlTestServer(t, media.URL)
	source := createControlTestSource(t, server, "live/concurrent-stop-delete", "", "")
	streamID := startControlTestSource(t, server, source.sourceID, http.StatusCreated)

	stopped := make(chan *httptest.ResponseRecorder, 1)
	go func() {
		stopped <- sourceRequest(t, server.handler(), http.MethodPost,
			"/api/sources/"+source.sourceID+"/stop", "", "")
	}()
	select {
	case <-deleteStarted:
	case <-time.After(time.Second):
		t.Fatal("stop cleanup did not start")
	}
	deleted := make(chan *httptest.ResponseRecorder, 1)
	go func() {
		deleted <- sourceRequest(t, server.handler(), http.MethodDelete,
			"/api/sources/"+source.sourceID, "", "")
	}()
	select {
	case response := <-deleted:
		close(releaseDelete)
		<-stopped
		t.Fatalf("source delete completed before stop cleanup: %d %s", response.Code, response.Body.String())
	case <-time.After(50 * time.Millisecond):
	}
	close(releaseDelete)

	if response := <-stopped; response.Code != http.StatusBadGateway {
		t.Fatalf("stop status/body = %d %s", response.Code, response.Body.String())
	}
	if response := <-deleted; response.Code != http.StatusOK {
		t.Fatalf("delete status/body = %d %s", response.Code, response.Body.String())
	}
	first := <-deletes
	second := <-deletes
	if first.streamID != streamID || second.streamID != streamID ||
		first.streamName != source.streamName || second.streamName != source.streamName {
		t.Fatalf("delete commands = %+v, %+v", first, second)
	}
	if _, err := server.sources.get(t.Context(), source.sourceID); !errors.Is(err, errSourceNotFound) {
		t.Fatalf("deleted source error = %v", err)
	}
	if _, ok := server.rtspSourceRuntime(source.sourceID); ok {
		t.Fatal("deleted source retained runtime ownership")
	}
	if _, bound := server.runtimes.sourceBinding(source.sourceID); bound {
		t.Fatal("deleted source retained runtime binding")
	}
	observed, ok := server.runtimes.byStreamID[streamID]
	if !ok || observed.State != "stopped" || observed.EndReason != "requested" {
		t.Fatalf("observed runtime = %+v, %v", observed, ok)
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

func waitSourceCreateRequest(t *testing.T, requests <-chan rtspPullCreateRequest) rtspPullCreateRequest {
	t.Helper()
	select {
	case request := <-requests:
		return request
	case <-time.After(time.Second):
		t.Fatal("media server create request timeout")
		return rtspPullCreateRequest{}
	}
}
