package main

import (
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"testing"
)

func TestWebAssetsAreEmbedded(t *testing.T) {
	previousDirectory, err := os.Getwd()
	if err != nil {
		t.Fatalf("Getwd() error = %v", err)
	}
	if err := os.Chdir(t.TempDir()); err != nil {
		t.Fatalf("Chdir() error = %v", err)
	}
	t.Cleanup(func() {
		if err := os.Chdir(previousDirectory); err != nil {
			t.Errorf("restore working directory error = %v", err)
		}
	})

	server := newTestInfrastructureServer(
		t, testConfig(), newMediaServerRegistry(), slog.New(slog.NewTextHandler(io.Discard, nil)))
	tests := []struct {
		path        string
		contentType string
		contains    string
	}{
		{path: "/", contentType: "text/html", contains: `href="/favicon.svg"`},
		{path: "/favicon.svg", contentType: "image/svg+xml", contains: "Media Control favicon"},
		{path: "/style.css", contentType: "text/css", contains: "--color-ink"},
		{path: "/app.js", contentType: "text/javascript", contains: `new EventSource("/api/events")`},
		{path: "/api.js", contentType: "text/javascript", contains: "/api/sources"},
		{path: "/whep.js", contentType: "text/javascript", contains: "X-Stream-ID"},
		{path: "/icons.svg", contentType: "image/svg+xml", contains: "Lucide icons"},
	}
	for _, test := range tests {
		t.Run(test.path, func(t *testing.T) {
			response := httptest.NewRecorder()
			request := httptest.NewRequest(http.MethodGet, test.path, nil)
			server.handler().ServeHTTP(response, request)
			if response.Code != http.StatusOK || !strings.HasPrefix(response.Header().Get("Content-Type"), test.contentType) ||
				!strings.Contains(response.Body.String(), test.contains) {
				t.Fatalf("response = %d %q %q", response.Code, response.Header().Get("Content-Type"), response.Body.String())
			}
		})
	}
}

func TestWebHandlerDoesNotShadowAPIRoutes(t *testing.T) {
	server := newTestInfrastructureServer(
		t, testConfig(), newMediaServerRegistry(), slog.New(slog.NewTextHandler(io.Discard, nil)))

	runtimeResponse := httptest.NewRecorder()
	server.handler().ServeHTTP(runtimeResponse, httptest.NewRequest(http.MethodGet, "/api/runtimes", nil))
	if runtimeResponse.Code != http.StatusOK || runtimeResponse.Header().Get("Content-Type") != "application/json" ||
		runtimeResponse.Body.String() != "{\"runtimes\":[]}\n" {
		t.Fatalf("runtime response = %d %q %q", runtimeResponse.Code, runtimeResponse.Header().Get("Content-Type"), runtimeResponse.Body.String())
	}

	missingResponse := httptest.NewRecorder()
	server.handler().ServeHTTP(missingResponse, httptest.NewRequest(http.MethodGet, "/missing.css", nil))
	if missingResponse.Code != http.StatusNotFound {
		t.Fatalf("missing asset status = %d", missingResponse.Code)
	}

	methodResponse := httptest.NewRecorder()
	server.handler().ServeHTTP(methodResponse, httptest.NewRequest(http.MethodPost, "/", nil))
	if methodResponse.Code != http.StatusMethodNotAllowed {
		t.Fatalf("root POST status = %d", methodResponse.Code)
	}
}
