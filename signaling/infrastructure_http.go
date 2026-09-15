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
	"net/url"
	"strings"
	"sync"
	"time"
)

type infrastructureServer struct {
	cfg                  config
	registry             *mediaServerRegistry
	logger               *slog.Logger
	live                 *liveService
	media                *mediaServerHTTPClient
	allocations          *publishAllocationRegistry
	sources              *sourceStore
	runtimes             *observedRuntimeRegistry
	runtimeEvents        *runtimeEventHub
	rtspPullMu           sync.Mutex
	rtspPulls            map[string]rtspPullRuntime
	sourceOperationMu    sync.Mutex
	sourceControlMu      sync.Mutex
	sourceControlWait    sync.WaitGroup
	sourceControlClosed  bool
	onMediaServerOffline func(mediaServerInstance)
}

func newInfrastructureServer(cfg config, registry *mediaServerRegistry, sources *sourceStore, logger *slog.Logger) *infrastructureServer {
	runtimeEvents := newRuntimeEventHub()
	runtimes := newObservedRuntimeRegistry()
	runtimes.setOnChange(runtimeEvents.publish)
	return &infrastructureServer{
		cfg: cfg, registry: registry, logger: logger,
		media: newMediaServerHTTPClient(cfg.mediaRequestTimeout), allocations: newPublishAllocationRegistry(),
		sources: sources, runtimes: runtimes, runtimeEvents: runtimeEvents, rtspPulls: make(map[string]rtspPullRuntime),
	}
}

func (s *infrastructureServer) handler() http.Handler {
	routes := http.NewServeMux()
	routes.HandleFunc("POST /internal/media-servers/register", s.handleMediaServerRegister)
	routes.HandleFunc("POST /internal/media-servers/heartbeat", s.handleMediaServerHeartbeat)
	routes.HandleFunc("POST /api/publish/allocations", s.handlePublishAllocation)
	routes.HandleFunc("GET /api/sources", s.handleSourceList)
	routes.HandleFunc("POST /api/sources", s.handleSourceCreate)
	routes.HandleFunc("PATCH /api/sources/{source_id}", s.handleSourcePatch)
	routes.HandleFunc("DELETE /api/sources/{source_id}", s.handleSourceDelete)
	routes.HandleFunc("POST /api/sources/{source_id}/start", s.handleSourceStart)
	routes.HandleFunc("POST /api/sources/{source_id}/stop", s.handleSourceStop)
	routes.HandleFunc("GET /api/media-servers", s.handleMediaServerList)
	routes.HandleFunc("GET /api/runtimes", s.handleRuntimeList)
	routes.HandleFunc("GET /api/events", s.handleRuntimeEvents)
	routes.HandleFunc("POST /api/preview/start", s.handlePreviewStart)
	routes.HandleFunc("POST /internal/publish/claim", s.handlePublishClaim)
	routes.HandleFunc("POST /internal/runtime-events", s.handleRuntimeEvent)
	if s.live != nil {
		routes.HandleFunc("POST /internal/live/start", s.handleLiveStart)
		routes.HandleFunc("POST /internal/live/stop", s.handleLiveStop)
		routes.HandleFunc("GET /api/devices", s.handleDeviceList)
		routes.HandleFunc("GET /api/devices/{device_id}/channels", s.handleChannelList)
		routes.HandleFunc("POST /api/devices/{device_id}/channels/{channel_id}/start", s.handleChannelLiveStart)
		routes.HandleFunc("POST /api/devices/{device_id}/channels/{channel_id}/stop", s.handleChannelLiveStop)
	}

	web := http.NewServeMux()
	web.Handle("GET /", embeddedWebHandler())
	return http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		if strings.HasPrefix(request.URL.Path, "/api/") || strings.HasPrefix(request.URL.Path, "/internal/") {
			routes.ServeHTTP(writer, request)
			return
		}
		web.ServeHTTP(writer, request)
	})
}

func (s *infrastructureServer) handleMediaServerRegister(writer http.ResponseWriter, request *http.Request) {
	var registration mediaServerRegistration
	if !decodeJSON(writer, request, &registration) || !validMediaServerRegistration(registration) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	registration.ControlURL = strings.TrimSuffix(registration.ControlURL, "/")
	if err := s.registry.register(registration, time.Now()); err != nil {
		writeHTTPError(writer, http.StatusConflict, "instance_conflict")
		return
	}
	writer.WriteHeader(http.StatusNoContent)
}

func (s *infrastructureServer) handleMediaServerHeartbeat(writer http.ResponseWriter, request *http.Request) {
	var heartbeat mediaServerHeartbeat
	if !decodeJSON(writer, request, &heartbeat) || heartbeat.ServerID == "" || heartbeat.InstanceID == "" {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	if err := s.registry.heartbeat(heartbeat.ServerID, heartbeat.InstanceID, time.Now()); err != nil {
		writeHTTPError(writer, http.StatusGone, "stale_instance")
		return
	}
	writer.WriteHeader(http.StatusNoContent)
}

func (s *infrastructureServer) serve(ctx context.Context) error {
	listener, err := net.Listen("tcp", s.cfg.httpListen)
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
		for {
			select {
			case <-serveContext.Done():
				s.runtimeEvents.close()
				shutdownContext, cancel := context.WithTimeout(context.Background(), 5*time.Second)
				if err := server.Shutdown(shutdownContext); err != nil {
					_ = server.Close()
				}
				cancel()
				return
			case now := <-ticker.C:
				s.allocations.expire(now)
				for _, instance := range s.registry.expire(now, s.cfg.mediaServerTimeout) {
					if s.onMediaServerOffline != nil {
						s.onMediaServerOffline(instance)
					}
				}
			}
		}
	}()
	err = server.Serve(listener)
	cancel()
	<-done
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
	decoder := json.NewDecoder(http.MaxBytesReader(writer, request.Body, 64*1024))
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

func validMediaServerRegistration(registration mediaServerRegistration) bool {
	if registration.ServerID == "" || registration.InstanceID == "" || len(registration.ServerID) > 128 || len(registration.InstanceID) > 128 {
		return false
	}
	controlURL, err := url.Parse(registration.ControlURL)
	if err != nil || controlURL.Scheme != "http" || controlURL.Host == "" || controlURL.User != nil || (controlURL.Path != "" && controlURL.Path != "/") || controlURL.RawQuery != "" || controlURL.Fragment != "" {
		return false
	}
	mediaIP := net.ParseIP(registration.MediaIP)
	return mediaIP != nil && !mediaIP.IsUnspecified() && registration.RTMPPort != 0 && registration.RTSPPort != 0 && registration.HTTPPort != 0
}

func writeHTTPError(writer http.ResponseWriter, status int, code string) {
	writeJSON(writer, status, map[string]string{"error": code})
}

func writeJSON(writer http.ResponseWriter, status int, value any) {
	writer.Header().Set("Content-Type", "application/json")
	writer.WriteHeader(status)
	_ = json.NewEncoder(writer).Encode(value)
}
