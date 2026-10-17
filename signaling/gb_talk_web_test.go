package main

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/gobwas/ws"
	"github.com/gobwas/ws/wsutil"
)

// 显式选择浏览器探针才运行；受控HTTP/WS故障与真实SIP整链测试分开标记。
func TestTalkWebLifecycle(t *testing.T) {
	probe := os.Getenv("GB_AUDIO_TEST_WEB_PROBE")
	if probe == "" {
		t.Skip("requires isolated Chrome audio-fixture probe")
	}
	assets := embeddedWebHandler()
	var packetMu sync.Mutex
	var packets int
	var lastPacket time.Time
	var maxGap time.Duration
	server := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		if request.URL.Path == "/fixture/packets" {
			packetMu.Lock()
			defer packetMu.Unlock()
			_ = json.NewEncoder(writer).Encode(map[string]any{"count": packets, "max_gap_ms": maxGap.Milliseconds()})
			return
		}
		if request.URL.Path == "/fixture" {
			writer.Header().Set("Content-Type", "text/html")
			_, _ = io.WriteString(writer, `<!doctype html><meta charset="utf-8"><button id="start" style="width:200px;height:60px">开始</button>`)
			return
		}
		if strings.HasPrefix(request.URL.Path, "/publish/gb28181/audio/"+strings.Repeat("5", 64)) {
			// 不依赖未读HTTP连接能否立即观测客户端断开；夹具自身也有有限期限。
			select {
			case <-time.After(6 * time.Second):
			case <-t.Context().Done():
			}
			http.Error(writer, "controlled handshake timeout", http.StatusGatewayTimeout)
			return
		}
		if strings.HasPrefix(request.URL.Path, "/publish/gb28181/audio/"+strings.Repeat("6", 64)) {
			connection, _, _, err := ws.UpgradeHTTP(request, writer)
			if err != nil {
				return
			}
			defer connection.Close()
			_ = connection.SetDeadline(time.Now().Add(3 * time.Second))
			if packet, _, err := wsutil.ReadClientData(connection); err != nil || len(packet) != 160 {
				t.Errorf("controlled peer did not receive PCMA: bytes=%d error=%v", len(packet), err)
			}
			_ = wsutil.WriteServerMessage(connection, ws.OpClose, nil)
			return
		}
		if strings.HasPrefix(request.URL.Path, "/publish/gb28181/audio/"+strings.Repeat("7", 64)) {
			connection, _, _, err := ws.UpgradeHTTP(request, writer)
			if err != nil {
				return
			}
			defer connection.Close()
			for {
				_ = connection.SetDeadline(time.Now().Add(5 * time.Second))
				packet, _, err := wsutil.ReadClientData(connection)
				if err != nil {
					return
				}
				if len(packet) != 160 {
					t.Errorf("unexpected PCMA packet size: %d", len(packet))
					return
				}
				now := time.Now()
				packetMu.Lock()
				if !lastPacket.IsZero() && now.Sub(lastPacket) > maxGap {
					maxGap = now.Sub(lastPacket)
				}
				lastPacket = now
				packets++
				packetMu.Unlock()
			}
		}
		assets.ServeHTTP(writer, request)
	}))
	defer server.Close()
	ctx, cancel := context.WithTimeout(t.Context(), 40*time.Second)
	defer cancel()
	command := exec.CommandContext(ctx, probe, server.URL, queryTestDeviceID)
	command.Env = append(os.Environ(), "WEB_CONSOLE_TALK_SCRIPT=tests/web_console/talk_lifecycle.mjs")
	output, err := command.CombinedOutput()
	t.Logf("browser lifecycle output: %s", output)
	if err != nil {
		t.Fatalf("browser lifecycle failed: %v", err)
	}
}
