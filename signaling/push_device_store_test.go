package main

import (
	"database/sql"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/google/uuid"
	"modernc.org/sqlite"
)

// 独立连接没有共享 token mutex：SQLite 条件 UPDATE 仍只允许一次消费，重开后不能复用。
func TestPushCredentialConcurrentConsumptionAcrossConnections(t *testing.T) {
	path := filepath.Join(t.TempDir(), "concurrent.db")
	first, err := openSourceStore(t.Context(), path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = first.close() })
	second, err := openSourceStore(t.Context(), path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = second.close() })
	now := time.Now()
	credential, err := newStreamToken()
	if err != nil {
		t.Fatal(err)
	}
	device := pushDevice{deviceID: uuid.NewString(), name: "Concurrent", protocol: pushProtocolRTMP, credential: credential, expiresAt: pushCredentialDeadline(now)}
	if err := first.createPushDevice(t.Context(), device); err != nil {
		t.Fatal(err)
	}
	type consumption struct {
		deviceID string
		err      error
	}
	results := make(chan consumption, 16)
	start := make(chan struct{})
	var wait sync.WaitGroup
	for i := range 16 {
		store := []*sourceStore{first, second}[i%2]
		wait.Go(func() {
			<-start
			id, err := store.consumePushCredential(t.Context(), credential, now)
			results <- consumption{deviceID: id, err: err}
		})
	}
	close(start)
	wait.Wait()
	close(results)
	successes := 0
	for result := range results {
		if result.err != nil {
			var sqliteError *sqlite.Error
			if !errors.As(result.err, &sqliteError) || sqliteError.Code() != 5 || result.deviceID != "" {
				t.Fatal(result.err)
			}
			// 独立连接不承诺忙等待；SQLITE_BUSY 是失败消费，不是第二次授权成功。
			continue
		}
		if result.deviceID != "" {
			if result.deviceID != device.deviceID {
				t.Fatal("consumed another device's credential")
			}
			successes++
		}
	}
	if successes != 1 {
		t.Fatalf("successful database consumptions=%d", successes)
	}
	for _, store := range []*sourceStore{first, second} {
		if id, err := store.consumePushCredential(t.Context(), credential, now); err != nil || id != "" {
			t.Fatalf("retry reused consumed credential: id=%s err=%v", id, err)
		}
	}
	reopened, err := openSourceStore(t.Context(), path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = reopened.close() })
	if id, err := reopened.consumePushCredential(t.Context(), credential, now); err != nil || id != "" {
		t.Fatalf("reopen reused consumed credential: id=%s err=%v", id, err)
	}
}

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
		if created.Protocol != protocol || created.State != "pending" || !validStreamToken(created.Token) {
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
		credential, _ := newStreamToken()
		if err := store.createPushDevice(t.Context(), pushDevice{deviceID: id, name: "Camera", protocol: protocol, credential: credential}); err != nil {
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

// 升级前的数据库只有 device_id 与 name：打开时补列，旧设备保持未指定且没有接入凭据，重复打开不重复修改。
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
		if err != nil || device.name != "Old encoder" || device.protocol != pushProtocolUnspecified || device.credential != "" {
			t.Fatalf("legacy device %d: %+v %v", attempt, device, err)
		}
		if attempt == 0 {
			credential, _ := newStreamToken()
			if err := store.createPushDevice(t.Context(), pushDevice{deviceID: uuid.NewString(), name: "New encoder", protocol: pushProtocolWHIP, credential: credential}); err != nil {
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

// 协议只用于展示与取舍接入地址：重新生成、验证、停止、删除与未指定协议的设备一致。
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
	if verified := decodePushDevice(t, apiRequest(t, handler, "GET", path, "", http.StatusOK).Body.String()); verified.State != "publishing" || verified.Protocol != pushProtocolWHIP {
		t.Fatalf("verified: %+v", verified)
	}
	apiRequest(t, handler, "POST", path+"/stop", "", http.StatusNoContent)
	if stopped := decodePushDevice(t, apiRequest(t, handler, "GET", path, "", http.StatusOK).Body.String()); stopped.State != "used" || stopped.Protocol != pushProtocolWHIP {
		t.Fatalf("stopped: %+v", stopped)
	}
	requestPushToken(t, s, device.DeviceID)
	apiRequest(t, handler, "DELETE", path, "", http.StatusNoContent)
	apiRequest(t, handler, "GET", path, "", http.StatusNotFound)
	if len(deleted) != 1 || deleted[0] != token {
		t.Fatalf("media receivers deleted: %v", deleted)
	}
}

func preparePushCredential(t *testing.T, handler http.Handler) pushURLs {
	t.Helper()
	var urls pushURLs
	response := apiRequest(t, handler, "POST", "/api/push-devices/credentials", "", http.StatusCreated)
	if err := json.Unmarshal(response.Body.Bytes(), &urls); err != nil || !validStreamToken(urls.Token) || !strings.HasSuffix(urls.RTMPURL, "/live/"+urls.Token) ||
		!strings.HasSuffix(urls.RTSPURL, "/"+urls.Token) || !strings.HasSuffix(urls.WHIPURL, "/publish/whip/"+urls.Token) {
		t.Fatalf("invalid prepared credential: %s", response.Body.String())
	}
	return urls
}

// 添加页面先展示真实地址，确认添加后才可发布：未确认、伪造或重复确认的凭据都不能使用。
func TestPushCredentialPrepareConfirmAndCancel(t *testing.T) {
	s := testInfrastructure(t)
	now := time.Now()
	s.live.sip.now = func() time.Time { return now }
	handler := s.handler()
	prepared := preparePushCredential(t, handler)
	verifyToken(t, s, prepared.Token, "publish", prepared.Token, http.StatusForbidden)
	forged, _ := newStreamToken()
	apiRequest(t, handler, "POST", "/api/push-devices", `{"name":"Forged","protocol":"rtmp","token":"`+forged+`"}`, http.StatusBadRequest)
	apiRequest(t, handler, "POST", "/api/push-devices", `{"name":"Bad","protocol":"rtmp","token":"short"}`, http.StatusBadRequest)

	now = now.Add(time.Minute)
	created := decodePushDevice(t, apiRequest(t, handler, "POST", "/api/push-devices", `{"name":"Camera","protocol":"rtsp","token":"`+prepared.Token+`"}`, http.StatusCreated).Body.String())
	if created.Token != prepared.Token || created.RTSPURL != prepared.RTSPURL || created.State != "pending" || !created.ExpiresAt.Equal(pushCredentialDeadline(now)) {
		t.Fatalf("confirmed device: %+v", created)
	}
	apiRequest(t, handler, "POST", "/api/push-devices", `{"name":"Duplicate","token":"`+prepared.Token+`"}`, http.StatusBadRequest)

	// 取消添加：预生成的凭据从未确认，确认期限过后丢弃，期间和之后都不能发布。
	cancelled := preparePushCredential(t, handler)
	verifyToken(t, s, cancelled.Token, "publish", cancelled.Token, http.StatusForbidden)
	now = now.Add(pushCredentialTTL)
	apiRequest(t, handler, "POST", "/api/push-devices", `{"name":"Late","token":"`+cancelled.Token+`"}`, http.StatusBadRequest)
	if _, kept := s.tokens.prepared[cancelled.Token]; kept {
		t.Fatal("expired prepared credential kept")
	}
	var list struct {
		Devices []pushDeviceResponse `json:"devices"`
	}
	if err := json.Unmarshal(apiRequest(t, handler, "GET", "/api/push-devices", "", http.StatusOK).Body.Bytes(), &list); err != nil || len(list.Devices) != 1 {
		t.Fatalf("cancelled add created a device: %+v %v", list, err)
	}
}

// 凭据从确认时起 5 分钟内可首次使用，到期即拒绝；首次认证后立即失效，已建立的推流不受到期影响。
func TestPushCredentialLifetimeAndSingleUse(t *testing.T) {
	s := testInfrastructure(t)
	now := time.Now()
	s.live.sip.now = func() time.Time { return now }
	handler := s.handler()
	expiring := decodePushDevice(t, apiRequest(t, handler, "POST", "/api/push-devices", `{"name":"Expiring","protocol":"rtmp"}`, http.StatusCreated).Body.String())
	used := decodePushDevice(t, apiRequest(t, handler, "POST", "/api/push-devices", `{"name":"Used","protocol":"whip"}`, http.StatusCreated).Body.String())

	now = now.Add(pushCredentialTTL - time.Millisecond)
	verifyToken(t, s, used.Token, "publish", used.Token, http.StatusOK)
	verifyToken(t, s, used.Token, "publish", used.Token, http.StatusForbidden)
	now = now.Add(time.Millisecond)
	verifyToken(t, s, expiring.Token, "publish", expiring.Token, http.StatusForbidden)
	if state := decodePushDevice(t, apiRequest(t, handler, "GET", "/api/push-devices/"+expiring.DeviceID, "", http.StatusOK).Body.String()).State; state != "expired" {
		t.Fatalf("expired state=%s", state)
	}
	now = now.Add(time.Hour)
	current := decodePushDevice(t, apiRequest(t, handler, "GET", "/api/push-devices/"+used.DeviceID, "", http.StatusOK).Body.String())
	if current.State != "publishing" || current.Token != used.Token || current.UsedAt.IsZero() {
		t.Fatalf("expiry ended an admitted publish: %+v", current)
	}
}

// 重新生成：旧凭据立即失效，新凭据重新计时；正在推流时拒绝且不停止现有发布；旧设备首次生成地址。
func TestPushCredentialRenew(t *testing.T) {
	s := testInfrastructure(t)
	now := time.Now()
	s.live.sip.now = func() time.Time { return now }
	media := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		t.Errorf("renew touched media server: %s", request.URL.Path)
	}))
	t.Cleanup(media.Close)
	s.media.server.controlURL = media.URL
	handler := s.handler()
	device := decodePushDevice(t, apiRequest(t, handler, "POST", "/api/push-devices", `{"name":"Camera","protocol":"rtmp"}`, http.StatusCreated).Body.String())
	path := "/api/push-devices/" + device.DeviceID
	now = now.Add(4 * time.Minute)
	renewed := decodePushDevice(t, apiRequest(t, handler, "POST", path+"/publish", "", http.StatusCreated).Body.String())
	if renewed.Token == device.Token || renewed.State != "pending" || !renewed.ExpiresAt.Equal(pushCredentialDeadline(now)) {
		t.Fatalf("renewed: %+v", renewed)
	}
	verifyToken(t, s, device.Token, "publish", device.Token, http.StatusForbidden)
	verifyToken(t, s, renewed.Token, "publish", renewed.Token, http.StatusOK)
	apiRequest(t, handler, "POST", path+"/publish", "", http.StatusConflict)
	if current := decodePushDevice(t, apiRequest(t, handler, "GET", path, "", http.StatusOK).Body.String()); current.State != "publishing" || current.Token != renewed.Token {
		t.Fatalf("conflicting renew changed publish: %+v", current)
	}

	legacyID := uuid.NewString()
	if _, err := s.sources.db.Exec(`INSERT INTO push_devices (device_id, name) VALUES (?, 'Old encoder')`, legacyID); err != nil {
		t.Fatal(err)
	}
	legacy := decodePushDevice(t, apiRequest(t, handler, "GET", "/api/push-devices/"+legacyID, "", http.StatusOK).Body.String())
	if legacy.State != "none" || legacy.Token != "" || legacy.RTMPURL != "" {
		t.Fatalf("legacy device: %+v", legacy)
	}
	if issued := decodePushDevice(t, apiRequest(t, handler, "POST", "/api/push-devices/"+legacyID+"/publish", "", http.StatusCreated).Body.String()); issued.State != "pending" || !validStreamToken(issued.Token) {
		t.Fatalf("legacy renew: %+v", issued)
	}
	apiRequest(t, handler, "POST", "/api/push-devices/"+uuid.NewString()+"/publish", "", http.StatusNotFound)
}

// 重启（含未正常关闭）后：未过期未使用的凭据仍可使用，已消费与已过期的凭据不可使用，不恢复推流运行。
func TestPushCredentialSurvivesRestart(t *testing.T) {
	path := filepath.Join(t.TempDir(), "signaling.db")
	store, err := openSourceStore(t.Context(), path)
	if err != nil {
		t.Fatal(err)
	}
	s := testInfrastructure(t)
	s.sources = store
	now := time.Now()
	s.live.sip.now = func() time.Time { return now }
	handler := s.handler()
	create := func(name string) pushDeviceResponse {
		return decodePushDevice(t, apiRequest(t, handler, "POST", "/api/push-devices", `{"name":"`+name+`","protocol":"rtmp"}`, http.StatusCreated).Body.String())
	}
	used, old := create("Used"), create("Old")
	verifyToken(t, s, used.Token, "publish", used.Token, http.StatusOK)
	now = now.Add(2 * time.Minute)
	fresh := create("Fresh")
	now = fresh.ExpiresAt.Add(-time.Minute)

	// 不关闭原连接直接重新打开，模拟 signaling 异常退出后重启。
	reopened, err := openSourceStore(t.Context(), path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = reopened.close(); _ = store.close() })
	restarted := newInfrastructureServer("127.0.0.1:0", reopened, s.live, s.media, s.logger)
	states := map[string]string{}
	var list struct {
		Devices []pushDeviceResponse `json:"devices"`
	}
	if err := json.Unmarshal(apiRequest(t, restarted.handler(), "GET", "/api/push-devices", "", http.StatusOK).Body.Bytes(), &list); err != nil {
		t.Fatal(err)
	}
	for _, device := range list.Devices {
		states[device.Name] = device.State
	}
	if states["Used"] != "used" || states["Old"] != "expired" || states["Fresh"] != "pending" {
		t.Fatalf("restart states: %v", states)
	}
	verifyToken(t, restarted, used.Token, "publish", used.Token, http.StatusForbidden)
	verifyToken(t, restarted, old.Token, "publish", old.Token, http.StatusForbidden)
	verifyToken(t, restarted, fresh.Token, "publish", fresh.Token, http.StatusOK)
	if len(restarted.tokens.pushRuns) != 1 {
		t.Fatalf("restart restored runs: %v", restarted.tokens.pushRuns)
	}
}

// dea4528 的数据库已有 protocol 列：补齐凭据列，凭据唯一。
func TestPushCredentialColumnsUpgradeProtocolDatabase(t *testing.T) {
	path := filepath.Join(t.TempDir(), "protocol.db")
	previous, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatal(err)
	}
	id := uuid.NewString()
	if _, err := previous.Exec(`CREATE TABLE push_devices (device_id TEXT PRIMARY KEY, name TEXT NOT NULL,
		protocol TEXT NOT NULL DEFAULT '' CHECK (protocol IN ('', 'rtmp', 'rtsp', 'whip')));
		INSERT INTO push_devices (device_id, name, protocol) VALUES (?, 'RTMP encoder', 'rtmp')`, id); err != nil {
		t.Fatal(err)
	}
	if err := previous.Close(); err != nil {
		t.Fatal(err)
	}
	credential, _ := newStreamToken()
	for attempt := 0; attempt < 2; attempt++ {
		store, err := openSourceStore(t.Context(), path)
		if err != nil {
			t.Fatalf("open %d: %v", attempt, err)
		}
		device, err := store.getPushDevice(t.Context(), id)
		if err != nil || device.protocol != pushProtocolRTMP || device.name != "RTMP encoder" {
			t.Fatalf("upgraded device %d: %+v %v", attempt, device, err)
		}
		if attempt == 0 {
			if device.credential != "" || !device.expiresAt.IsZero() || !device.usedAt.IsZero() {
				t.Fatalf("upgrade invented a credential: %+v", device)
			}
			if err := store.renewPushCredential(t.Context(), id, credential, time.Now().Add(pushCredentialTTL)); err != nil {
				t.Fatal(err)
			}
		} else if device.credential != credential {
			t.Fatalf("credential lost on reopen: %+v", device)
		}
		duplicate := pushDevice{deviceID: uuid.NewString(), name: "Copy", credential: credential, expiresAt: time.Now()}
		if err := store.createPushDevice(t.Context(), duplicate); err == nil {
			t.Fatal("duplicate credential accepted")
		}
		_ = store.close()
	}
}
