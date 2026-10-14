package main

import (
	"database/sql"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"testing"

	"github.com/google/uuid"
)

func decodePushDevice(t *testing.T, body string) pushDeviceResponse {
	t.Helper()
	var device pushDeviceResponse
	if err := json.Unmarshal([]byte(body), &device); err != nil {
		t.Fatalf("invalid push device: %s", body)
	}
	return device
}

func TestPushDeviceProtocolCreateAndValidate(t *testing.T) {
	s := testInfrastructure(t)
	handler := s.handler()
	for _, protocol := range []pushProtocol{pushProtocolRTMP, pushProtocolRTSP, pushProtocolWHIP} {
		created := decodePushDevice(t, apiRequest(t, handler, "POST", "/api/push-devices", `{"name":"Camera `+string(protocol)+`","protocol":"`+string(protocol)+`"}`, http.StatusCreated).Body.String())
		if created.Protocol != protocol || created.State != "idle" {
			t.Fatalf("create %s: %+v", protocol, created)
		}
		fetched := decodePushDevice(t, apiRequest(t, handler, "GET", "/api/push-devices/"+created.DeviceID, "", http.StatusOK).Body.String())
		if fetched.Protocol != protocol {
			t.Fatalf("get %s: %+v", protocol, fetched)
		}
	}
	for _, body := range []string{`{"name":"Bad","protocol":"srt"}`, `{"name":"Bad","protocol":"RTMP"}`, `{"name":"Bad","protocol":1}`} {
		apiRequest(t, handler, "POST", "/api/push-devices", body, http.StatusBadRequest)
	}
	unspecified := decodePushDevice(t, apiRequest(t, handler, "POST", "/api/push-devices", `{"name":"Legacy client"}`, http.StatusCreated).Body.String())
	if unspecified.Protocol != pushProtocolUnspecified || !strings.Contains(apiRequest(t, handler, "GET", "/api/push-devices/"+unspecified.DeviceID, "", http.StatusOK).Body.String(), `"protocol":""`) {
		t.Fatalf("missing protocol must stay unspecified: %+v", unspecified)
	}
	var list struct {
		Devices []pushDeviceResponse `json:"devices"`
	}
	if err := json.Unmarshal(apiRequest(t, handler, "GET", "/api/push-devices", "", http.StatusOK).Body.Bytes(), &list); err != nil || len(list.Devices) != 4 {
		t.Fatalf("list: %+v %v", list, err)
	}
}

func TestPushDeviceProtocolPatch(t *testing.T) {
	s := testInfrastructure(t)
	handler := s.handler()
	device := decodePushDevice(t, apiRequest(t, handler, "POST", "/api/push-devices", `{"name":"Camera","protocol":"rtmp"}`, http.StatusCreated).Body.String())
	path := "/api/push-devices/" + device.DeviceID
	renamed := decodePushDevice(t, apiRequest(t, handler, "PATCH", path, `{"name":"Renamed"}`, http.StatusOK).Body.String())
	if renamed.Name != "Renamed" || renamed.Protocol != pushProtocolRTMP {
		t.Fatalf("rename must keep protocol: %+v", renamed)
	}
	changed := decodePushDevice(t, apiRequest(t, handler, "PATCH", path, `{"name":"Renamed","protocol":"whip"}`, http.StatusOK).Body.String())
	if changed.Protocol != pushProtocolWHIP {
		t.Fatalf("protocol change: %+v", changed)
	}
	apiRequest(t, handler, "PATCH", path, `{"name":"Renamed","protocol":"srt"}`, http.StatusBadRequest)
	if current := decodePushDevice(t, apiRequest(t, handler, "GET", path, "", http.StatusOK).Body.String()); current.Protocol != pushProtocolWHIP {
		t.Fatalf("rejected patch changed protocol: %+v", current)
	}
}

func TestPushDeviceProtocolPersistsAcrossReopen(t *testing.T) {
	path := filepath.Join(t.TempDir(), "signaling.db")
	store, err := openSourceStore(t.Context(), path)
	if err != nil {
		t.Fatal(err)
	}
	devices := map[string]pushProtocol{uuid.NewString(): pushProtocolRTMP, uuid.NewString(): pushProtocolRTSP, uuid.NewString(): pushProtocolWHIP, uuid.NewString(): pushProtocolUnspecified}
	for id, protocol := range devices {
		if err := store.createPushDevice(t.Context(), pushDevice{deviceID: id, name: "Camera", protocol: protocol}); err != nil {
			t.Fatal(err)
		}
	}
	if err := store.close(); err != nil {
		t.Fatal(err)
	}
	reopened, err := openSourceStore(t.Context(), path)
	if err != nil {
		t.Fatal(err)
	}
	defer reopened.close()
	for id, protocol := range devices {
		device, err := reopened.getPushDevice(t.Context(), id)
		if err != nil || device.protocol != protocol {
			t.Fatalf("reopen %s: %+v %v", id, device, err)
		}
	}
}

// 升级前的数据库只有 device_id 与 name：打开时补列，旧设备保持未指定，重复打开不重复修改。
func TestPushDeviceProtocolUpgradesLegacyDatabase(t *testing.T) {
	path := filepath.Join(t.TempDir(), "legacy.db")
	legacy, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatal(err)
	}
	legacyID := uuid.NewString()
	if _, err := legacy.Exec(`CREATE TABLE push_devices (device_id TEXT PRIMARY KEY, name TEXT NOT NULL);
		INSERT INTO push_devices (device_id, name) VALUES (?, 'Old encoder')`, legacyID); err != nil {
		t.Fatal(err)
	}
	if err := legacy.Close(); err != nil {
		t.Fatal(err)
	}
	for attempt := 0; attempt < 2; attempt++ {
		store, err := openSourceStore(t.Context(), path)
		if err != nil {
			t.Fatalf("open %d: %v", attempt, err)
		}
		device, err := store.getPushDevice(t.Context(), legacyID)
		if err != nil || device.name != "Old encoder" || device.protocol != pushProtocolUnspecified {
			t.Fatalf("legacy device %d: %+v %v", attempt, device, err)
		}
		if attempt == 0 {
			if err := store.createPushDevice(t.Context(), pushDevice{deviceID: uuid.NewString(), name: "New encoder", protocol: pushProtocolWHIP}); err != nil {
				t.Fatal(err)
			}
		}
		listed, err := store.listPushDevices(t.Context())
		if err != nil || len(listed) != 2 {
			t.Fatalf("list %d: %+v %v", attempt, listed, err)
		}
		if _, err := store.db.Exec(`UPDATE push_devices SET protocol = 'srt' WHERE device_id = ?`, legacyID); err == nil {
			t.Fatal("protocol column must reject unknown values")
		}
		_ = store.close()
	}
}

// 协议只用于展示与取舍签发结果：签发、验证、停止、删除与未指定协议的设备一致。
func TestPushDeviceProtocolKeepsPublishLifecycle(t *testing.T) {
	s := testInfrastructure(t)
	var deleted []string
	media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		var body struct {
			StreamID string `json:"stream_id"`
		}
		if request.URL.Path != "/receivers/delete" || json.NewDecoder(request.Body).Decode(&body) != nil {
			t.Errorf("invalid media request: %s", request.URL.Path)
		}
		deleted = append(deleted, body.StreamID)
		writer.WriteHeader(http.StatusNoContent)
	}))
	t.Cleanup(media.Close)
	s.media.server.controlURL = media.URL
	handler := s.handler()
	device := decodePushDevice(t, apiRequest(t, handler, "POST", "/api/push-devices", `{"name":"Camera","protocol":"whip"}`, http.StatusCreated).Body.String())
	path := "/api/push-devices/" + device.DeviceID
	published := apiRequest(t, handler, "POST", path+"/publish", "", http.StatusCreated).Body.String()
	for _, field := range []string{`"rtmp_url"`, `"rtsp_url"`, `"whip_url"`} {
		if !strings.Contains(published, field) {
			t.Fatalf("publish response missing %s: %s", field, published)
		}
	}
	token := decodePushDevice(t, apiRequest(t, handler, "GET", path, "", http.StatusOK).Body.String()).Token
	verifyToken(t, s, token, "publish", token, http.StatusOK)
	if verified := decodePushDevice(t, apiRequest(t, handler, "GET", path, "", http.StatusOK).Body.String()); verified.State != "verified" || verified.Protocol != pushProtocolWHIP {
		t.Fatalf("verified: %+v", verified)
	}
	apiRequest(t, handler, "POST", path+"/stop", "", http.StatusNoContent)
	if stopped := decodePushDevice(t, apiRequest(t, handler, "GET", path, "", http.StatusOK).Body.String()); stopped.State != "idle" || stopped.Protocol != pushProtocolWHIP {
		t.Fatalf("stopped: %+v", stopped)
	}
	requestPushToken(t, s, device.DeviceID)
	apiRequest(t, handler, "DELETE", path, "", http.StatusNoContent)
	apiRequest(t, handler, "GET", path, "", http.StatusNotFound)
	if len(deleted) != 2 || deleted[0] != token {
		t.Fatalf("media receivers deleted: %v", deleted)
	}
}
