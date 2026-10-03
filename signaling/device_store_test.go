package main

import (
	"errors"
	"path/filepath"
	"testing"
)

func TestDeviceStorePersistence(t *testing.T) {
	path := filepath.Join(t.TempDir(), "devices.db")
	sources, err := openSourceStore(t.Context(), path)
	if err != nil {
		t.Fatal(err)
	}
	store, err := newDeviceStore(t.Context(), sources.db)
	if err != nil {
		t.Fatal(err)
	}
	device := gbDevice{deviceID: "34020000001320000001", name: "大门摄像机"}
	if err := store.create(t.Context(), device); err != nil {
		t.Fatal(err)
	}
	if err := store.create(t.Context(), device); !errors.Is(err, errDeviceExists) {
		t.Fatalf("duplicate: %v", err)
	}
	if err := sources.close(); err != nil {
		t.Fatal(err)
	}
	sources, err = openSourceStore(t.Context(), path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = sources.close() })
	store, err = newDeviceStore(t.Context(), sources.db)
	if err != nil {
		t.Fatal(err)
	}
	got, err := store.get(t.Context(), device.deviceID)
	if err != nil || got != device {
		t.Fatalf("get after reopen: %+v, %v", got, err)
	}
	devices, err := store.list(t.Context())
	if err != nil || len(devices) != 1 || devices[0] != device {
		t.Fatalf("list: %+v, %v", devices, err)
	}
	if exists, err := store.exists(t.Context(), device.deviceID); err != nil || !exists {
		t.Fatalf("exists: %v, %v", exists, err)
	}
	if err := store.delete(t.Context(), device.deviceID); err != nil {
		t.Fatal(err)
	}
	if exists, err := store.exists(t.Context(), device.deviceID); err != nil || exists {
		t.Fatalf("deleted exists: %v, %v", exists, err)
	}
	if _, err := store.get(t.Context(), device.deviceID); !errors.Is(err, errDeviceNotFound) {
		t.Fatalf("deleted get: %v", err)
	}
	if err := store.delete(t.Context(), device.deviceID); !errors.Is(err, errDeviceNotFound) {
		t.Fatalf("deleted delete: %v", err)
	}
}

func TestDeviceStoreValidation(t *testing.T) {
	sources, err := openSourceStore(t.Context(), filepath.Join(t.TempDir(), "devices.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = sources.close() })
	store, err := newDeviceStore(t.Context(), sources.db)
	if err != nil {
		t.Fatal(err)
	}
	for _, device := range []gbDevice{
		{deviceID: "123", name: "camera"},
		{deviceID: "3402000000132000000a", name: "camera"},
		{deviceID: "34020000001320000001", name: ""},
		{deviceID: "34020000001320000001", name: "   "},
	} {
		if err := store.create(t.Context(), device); !errors.Is(err, errInvalidDevice) {
			t.Fatalf("accepted %+v: %v", device, err)
		}
	}
}
