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
	"time"
)

type infrastructureServer struct {
	cfg                  config
	registry             *mediaServerRegistry
	logger               *slog.Logger
	onMediaServerOffline func(mediaServerInstance)
}

func newInfrastructureServer(cfg config, registry *mediaServerRegistry, logger *slog.Logger) *infrastructureServer {
	return &infrastructureServer{cfg: cfg, registry: registry, logger: logger}
}

func (s *infrastructureServer) handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("POST /internal/media-servers/register", s.handleMediaServerRegister)
	mux.HandleFunc("POST /internal/media-servers/heartbeat", s.handleMediaServerHeartbeat)
	return mux
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
	writeJSON(writer, http.StatusOK, map[string]string{"result": "ok"})
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
	writeJSON(writer, http.StatusOK, map[string]string{"result": "ok"})
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
				shutdownContext, cancel := context.WithTimeout(context.Background(), 5*time.Second)
				_ = server.Shutdown(shutdownContext)
				cancel()
				return
			case now := <-ticker.C:
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

func validMediaServerRegistration(registration mediaServerRegistration) bool {
	if registration.ServerID == "" || registration.InstanceID == "" || len(registration.ServerID) > 128 || len(registration.InstanceID) > 128 {
		return false
	}
	controlURL, err := url.Parse(registration.ControlURL)
	if err != nil || controlURL.Scheme != "http" || controlURL.Host == "" || controlURL.User != nil || (controlURL.Path != "" && controlURL.Path != "/") || controlURL.RawQuery != "" || controlURL.Fragment != "" {
		return false
	}
	mediaIP := net.ParseIP(registration.MediaIP)
	return mediaIP != nil && !mediaIP.IsUnspecified()
}

func writeHTTPError(writer http.ResponseWriter, status int, code string) {
	writeJSON(writer, status, map[string]string{"error": code})
}

func writeJSON(writer http.ResponseWriter, status int, value any) {
	writer.Header().Set("Content-Type", "application/json")
	writer.WriteHeader(status)
	_ = json.NewEncoder(writer).Encode(value)
}
