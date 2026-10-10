package main

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"mime"
	"net"
	"net/http"
	"strings"
	"sync"
	"time"
)

type infrastructureServer struct {
	httpListen          string
	logger              *slog.Logger
	live                *liveService
	media               *mediaServerHTTPClient
	sources             *sourceStore
	rtspPullMu          sync.Mutex
	rtspPulls           map[string]rtspPullSession
	sourceOperationMu   sync.Mutex
	sourceControlMu     sync.Mutex
	sourceControlWait   sync.WaitGroup
	sourceControlClosed bool
	tokens              *streamTokens
	tokenTTL            time.Duration
}

func newInfrastructureServer(
	httpListen string,
	sources *sourceStore,
	live *liveService,
	media *mediaServerHTTPClient,
	logger *slog.Logger,
) *infrastructureServer {
	tokens := &streamTokens{tokens: make(map[string]streamToken), pushRuns: make(map[string]pushRun)}
	live.tokens = tokens
	return &infrastructureServer{
		httpListen: httpListen, logger: logger, live: live, media: media,
		sources: sources, rtspPulls: make(map[string]rtspPullSession),
		tokens: tokens, tokenTTL: 60 * time.Second,
	}
}

func (s *infrastructureServer) handler() http.Handler {
	routes := http.NewServeMux()
	routes.HandleFunc("GET /api/sources", s.handleSourceList)
	routes.HandleFunc("POST /api/sources", s.handleSourceCreate)
	routes.HandleFunc("PATCH /api/sources/{source_id}", s.handleSourcePatch)
	routes.HandleFunc("DELETE /api/sources/{source_id}", s.handleSourceDelete)
	routes.HandleFunc("POST /api/sources/{source_id}/start", s.handleSourceStart)
	routes.HandleFunc("POST /api/sources/{source_id}/stop", s.handleSourceStop)
	routes.HandleFunc("POST /api/preview/start", s.handlePreviewStart)
	routes.HandleFunc("GET /api/devices", s.handleDeviceList)
	routes.HandleFunc("POST /api/devices", s.handleDeviceCreate)
	routes.HandleFunc("GET /api/devices/{device_id}", s.handleDeviceGet)
	routes.HandleFunc("DELETE /api/devices/{device_id}", s.handleDeviceDelete)
	routes.HandleFunc("GET /api/devices/{device_id}/channels", s.handleChannelList)
	routes.HandleFunc("POST /api/devices/{device_id}/channels/{channel_id}/play", s.handleChannelPlay)
	routes.HandleFunc("POST /api/play", s.handlePlay)
	routes.HandleFunc("POST /internal/verify", s.handleVerify)
	routes.HandleFunc("GET /api/push-devices", s.handlePushDeviceList)
	routes.HandleFunc("POST /api/push-devices", s.handlePushDeviceCreate)
	routes.HandleFunc("GET /api/push-devices/{id}", s.handlePushDeviceGet)
	routes.HandleFunc("PATCH /api/push-devices/{id}", s.handlePushDevicePatch)
	routes.HandleFunc("DELETE /api/push-devices/{id}", s.handlePushDeviceDelete)
	routes.HandleFunc("POST /api/push-devices/{id}/publish", s.handlePushPublish)
	routes.HandleFunc("POST /api/push-devices/{id}/stop", s.handlePushStop)
	routes.HandleFunc("DELETE /api/lives/{live_id}", s.handleLiveDelete)

	web := embeddedWebHandler()
	routes.Handle("GET /{$}", web)
	routes.Handle("GET /{asset}", web)
	return routes
}

func (s *infrastructureServer) serve(ctx context.Context) error {
	listener, err := net.Listen("tcp", s.httpListen)
	if err != nil {
		return err
	}
	server := &http.Server{
		Handler:           s.handler(),
		ReadHeaderTimeout: 5 * time.Second,
	}
	serveContext, cancel := context.WithCancel(ctx)
	done := make(chan struct{})
	go func() {
		defer close(done)
		ticker := time.NewTicker(time.Second)
		defer ticker.Stop()
	sweep:
		for {
			select {
			case <-serveContext.Done():
				break sweep
			case now := <-ticker.C:
				s.tokens.expire(now)
			}
		}
		shutdownContext, shutdownCancel := context.WithTimeout(context.Background(), 5*time.Second)
		if err := server.Shutdown(shutdownContext); err != nil {
			_ = server.Close()
		}
		shutdownCancel()
	}()
	reconcileDone := make(chan struct{})
	go func() {
		defer close(reconcileDone)
		s.runMediaReconcile(serveContext)
	}()
	err = server.Serve(listener)
	cancel()
	<-done
	<-reconcileDone
	if errors.Is(err, http.ErrServerClosed) && ctx.Err() != nil {
		return nil
	}
	return err
}

func decodeJSON(writer http.ResponseWriter, request *http.Request, target any) bool {
	mediaType, _, err := mime.ParseMediaType(request.Header.Get("Content-Type"))
	if err != nil || !strings.EqualFold(mediaType, "application/json") {
		return false
	}
	decoder := json.NewDecoder(http.MaxBytesReader(writer, request.Body, 512*1024))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(target); err != nil {
		return false
	}
	var extra any
	return decoder.Decode(&extra) == io.EOF
}

func decodeOptionalString(raw json.RawMessage) (*string, bool) {
	if raw == nil {
		return nil, true
	}
	var value *string
	if err := json.Unmarshal(raw, &value); err != nil {
		return nil, false
	}
	return value, value != nil
}

func writeHTTPError(writer http.ResponseWriter, status int, code string) {
	writeJSON(writer, status, map[string]string{"error": code})
}

func writeJSON(writer http.ResponseWriter, status int, value any) {
	writer.Header().Set("Content-Type", "application/json")
	writer.WriteHeader(status)
	_ = json.NewEncoder(writer).Encode(value)
}
