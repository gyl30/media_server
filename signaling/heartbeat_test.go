package main

import (
	"fmt"
	"testing"
	"time"

	"github.com/emiago/sipgo/sip"
)

func TestKeepaliveMessageRefreshesDevice(t *testing.T) {
	cfg := testConfig()
	server, addr := startRegistrar(t, cfg)
	client := newRegisterClient(t)
	registerDevice(t, client, cfg, addr, testDeviceID, 120)
	before, _ := server.devices.get(testDeviceID)

	request := newMessageRequest(t, cfg, addr, testDeviceID, keepaliveXML(testDeviceID, "Keepalive", "OK"))
	response := doSIP(t, client, request)
	if response.StatusCode != sip.StatusOK {
		t.Fatalf("keepalive status = %d", response.StatusCode)
	}
	after, ok := server.devices.get(testDeviceID)
	if !ok || !after.online || after.lastHeartbeat.Before(before.lastHeartbeat) || after.lastHeartbeat.IsZero() {
		t.Fatalf("device after keepalive = %+v", after)
	}
}

func TestKeepaliveMessageRejectsInvalidPayload(t *testing.T) {
	cfg := testConfig()
	_, addr := startRegistrar(t, cfg)
	client := newRegisterClient(t)
	registerDevice(t, client, cfg, addr, testDeviceID, 120)

	for name, body := range map[string][]byte{
		"wrong command": keepaliveXML(testDeviceID, "Alarm", "OK"),
		"wrong device":  keepaliveXML("34020000001320000002", "Keepalive", "OK"),
		"wrong status":  keepaliveXML(testDeviceID, "Keepalive", "ERROR"),
		"invalid XML":   []byte("<Notify><CmdType>Keepalive"),
		"trailing XML":  append(keepaliveXML(testDeviceID, "Keepalive", "OK"), []byte("<Extra/>")...),
	} {
		t.Run(name, func(t *testing.T) {
			response := doSIP(t, client, newMessageRequest(t, cfg, addr, testDeviceID, body))
			if response.StatusCode == sip.StatusOK {
				t.Fatalf("status = %d", response.StatusCode)
			}
		})
	}
}

func TestKeepaliveRequiresRegisteredSource(t *testing.T) {
	cfg := testConfig()
	_, addr := startRegistrar(t, cfg)
	client := newRegisterClient(t)
	response := doSIP(t, client, newMessageRequest(t, cfg, addr, testDeviceID, keepaliveXML(testDeviceID, "Keepalive", "OK")))
	if response.StatusCode != sip.StatusForbidden {
		t.Fatalf("status = %d", response.StatusCode)
	}
}

func TestDeviceTimeoutTransitionsOfflineOnce(t *testing.T) {
	now := time.Date(2026, 8, 29, 12, 0, 0, 0, time.UTC)
	registry := newDeviceRegistry()
	registry.register(registeredDevice{
		id:            testDeviceID,
		expiresAt:     now.Add(time.Hour),
		lastHeartbeat: now,
		online:        true,
	})
	var offline []string
	server := sipServer{
		cfg:             config{heartbeatTimeout: time.Minute},
		devices:         registry,
		channels:        newChannelRegistry(),
		onDeviceOffline: func(deviceID string) { offline = append(offline, deviceID) },
	}
	server.expireDevices(now.Add(time.Minute))
	server.expireDevices(now.Add(2 * time.Minute))
	device, ok := registry.get(testDeviceID)
	if !ok || device.online {
		t.Fatalf("device = %+v, exists = %v", device, ok)
	}
	if len(offline) != 1 || offline[0] != testDeviceID {
		t.Fatalf("offline callbacks = %v", offline)
	}
}

func TestRegistrationExpiryTransitionsOfflineOnce(t *testing.T) {
	now := time.Date(2026, 8, 29, 12, 0, 0, 0, time.UTC)
	registry := newDeviceRegistry()
	registry.register(registeredDevice{
		id:            testDeviceID,
		expiresAt:     now.Add(time.Minute),
		lastHeartbeat: now,
		online:        true,
	})
	var offline []string
	server := sipServer{
		cfg:             config{heartbeatTimeout: time.Hour},
		devices:         registry,
		channels:        newChannelRegistry(),
		onDeviceOffline: func(deviceID string) { offline = append(offline, deviceID) },
	}
	server.expireDevices(now.Add(time.Minute))
	server.expireDevices(now.Add(2 * time.Minute))
	if _, ok := registry.get(testDeviceID); ok {
		t.Fatal("expired registration remains")
	}
	if len(offline) != 1 || offline[0] != testDeviceID {
		t.Fatalf("offline callbacks = %v", offline)
	}
}

func TestDeviceOfflineRemovesChannelsAndCatalogPending(t *testing.T) {
	server := newTestSIPServer(t, "127.0.0.1:0")
	registerLiveTestDevice(t, server, "127.0.0.1:5062")
	server.channels.beginQuery(testDeviceID, 9)
	server.onDeviceOffline = func(deviceID string) { server.channels.removeDevice(deviceID) }
	server.notifyDeviceOffline(testDeviceID)
	if server.channels.len(testDeviceID) != 0 || server.channels.pendingLen() != 0 {
		t.Fatalf("channels=%d pending=%d", server.channels.len(testDeviceID), server.channels.pendingLen())
	}
}

func TestKeepaliveDoesNotReviveExpiredRegistration(t *testing.T) {
	now := time.Date(2026, 8, 29, 12, 0, 0, 0, time.UTC)
	registry := newDeviceRegistry()
	registry.register(registeredDevice{
		id: testDeviceID, remoteEndpoint: "127.0.0.1:5062", expiresAt: now, lastHeartbeat: now.Add(-time.Minute), online: false,
	})
	if registry.keepalive(testDeviceID, "127.0.0.1:5062", now) {
		t.Fatal("expired registration accepted Keepalive")
	}
}

func TestMessageRejectsWrongPlatformTarget(t *testing.T) {
	cfg := testConfig()
	_, addr := startRegistrar(t, cfg)
	client := newRegisterClient(t)
	registerDevice(t, client, cfg, addr, testDeviceID, 120)
	request := newMessageRequest(t, cfg, addr, testDeviceID, keepaliveXML(testDeviceID, "Keepalive", "OK"))
	request.Recipient.User = "34020000002000000009"
	if response := doSIP(t, client, request); response.StatusCode != sip.StatusBadRequest {
		t.Fatalf("status = %d", response.StatusCode)
	}
}

func newMessageRequest(t *testing.T, cfg config, addr, deviceID string, body []byte) *sip.Request {
	t.Helper()
	recipient := recipientFor(t, addr)
	recipient.User = cfg.sipID
	request := sip.NewRequest(sip.MESSAGE, recipient)
	fromParams := sip.NewParams()
	fromParams.Add("tag", fmt.Sprintf("message-%d", time.Now().UnixNano()))
	request.AppendHeader(&sip.FromHeader{
		Address: sip.Uri{Scheme: "sip", User: deviceID, Host: cfg.sipDomain},
		Params:  fromParams,
	})
	request.AppendHeader(&sip.ToHeader{Address: sip.Uri{Scheme: "sip", User: cfg.sipID, Host: cfg.sipDomain}})
	request.AppendHeader(sip.NewHeader("Content-Type", "Application/MANSCDP+xml"))
	request.SetBody(body)
	request.SetTransport("UDP")
	return request
}

func keepaliveXML(deviceID, cmdType, status string) []byte {
	return fmt.Appendf(nil, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n<Notify><CmdType>%s</CmdType><SN>1</SN><DeviceID>%s</DeviceID><Status>%s</Status></Notify>", cmdType, deviceID, status)
}
