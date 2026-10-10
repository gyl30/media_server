package main

import (
	"mime"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestWebTokenManagement(t *testing.T) {
	handler := testInfrastructure(t).handler()
	for path, snippets := range map[string][]string{
		"/":        {`id="view-push"`, `id="push-form"`, `id="url-dialog"`},
		"/api.js":  {`"/api/play"`, `"/api/push-devices"`},
		"/whep.js": {`ticket.stream_id`, `api.play(`, `new URL(location, session.whepURL)`},
	} {
		response := httptest.NewRecorder()
		handler.ServeHTTP(response, httptest.NewRequest("GET", path, nil))
		for _, snippet := range snippets {
			if !strings.Contains(response.Body.String(), snippet) {
				t.Errorf("%s missing %s", path, snippet)
			}
		}
		if strings.Contains(response.Body.String(), "/play/whep/${") || strings.Contains(response.Body.String(), "ticket.live_id") {
			t.Errorf("%s retains proxy/ticket contract", path)
		}
	}
}

func TestWebManagementPageAndAssets(t *testing.T) {
	handler := testInfrastructure(t).handler()
	for path, expected := range map[string]string{
		"/": "text/html", "/app.js": "text/javascript", "/api.js": "text/javascript",
		"/whep.js": "text/javascript", "/style.css": "text/css",
		"/icons.svg": "image/svg+xml", "/favicon.svg": "image/svg+xml",
	} {
		t.Run(path, func(t *testing.T) {
			response := httptest.NewRecorder()
			handler.ServeHTTP(response, httptest.NewRequest("GET", path, nil))
			mediaType, _, err := mime.ParseMediaType(response.Header().Get("Content-Type"))
			if response.Code != http.StatusOK || err != nil || mediaType != expected || response.Body.Len() == 0 {
				t.Fatalf("GET %s: status=%d type=%s error=%v", path, response.Code, mediaType, err)
			}
			if path == "/" && (!strings.Contains(response.Body.String(), `lang="zh-CN"`) || !strings.Contains(response.Body.String(), "添加设备")) {
				t.Fatal("首页未提供中文设备管理入口")
			}
		})
	}
	for _, path := range []string{"/missing.js", "/missing/asset.js"} {
		response := httptest.NewRecorder()
		handler.ServeHTTP(response, httptest.NewRequest("GET", path, nil))
		if response.Code != http.StatusNotFound {
			t.Fatalf("GET %s: %d", path, response.Code)
		}
	}
}
