package main

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"testing"

	"github.com/google/uuid"
)

func TestSourceStorePersistsAndUpdatesSources(t *testing.T) {
	ctx := context.Background()
	path := filepath.Join(t.TempDir(), "signaling.db")
	store, err := openSourceStore(ctx, path)
	if err != nil {
		t.Fatalf("openSourceStore() error = %v", err)
	}
	first := rtspSource{
		sourceID: uuid.NewString(), streamName: "live/b", url: "rtsp://camera.example/b",
		username: "admin", password: "secret", desiredState: sourceDesiredRunning,
	}
	second := rtspSource{
		sourceID: uuid.NewString(), streamName: "live/a", url: "rtsp://camera.example/a",
		desiredState: sourceDesiredStopped,
	}
	if err := store.create(ctx, first); err != nil {
		t.Fatalf("create(first) error = %v", err)
	}
	if err := store.create(ctx, second); err != nil {
		t.Fatalf("create(second) error = %v", err)
	}
	sources, err := store.list(ctx)
	if err != nil {
		t.Fatalf("list() error = %v", err)
	}
	if len(sources) != 2 || sources[0].sourceID != second.sourceID || sources[1].sourceID != first.sourceID {
		t.Fatalf("list() = %+v", sources)
	}

	updatedURL := "rtsp://camera.example/updated"
	updated, err := store.patch(ctx, first.sourceID, rtspSourcePatch{url: &updatedURL})
	if err != nil {
		t.Fatalf("patch(url) error = %v", err)
	}
	if updated.url != updatedURL || updated.password != "secret" {
		t.Fatalf("patch(url) = %+v", updated)
	}
	empty := ""
	updated, err = store.patch(ctx, first.sourceID, rtspSourcePatch{username: &empty, password: &empty})
	if err != nil {
		t.Fatalf("patch(clear credentials) error = %v", err)
	}
	if updated.username != "" || updated.password != "" {
		t.Fatalf("cleared credentials = %q/%q", updated.username, updated.password)
	}
	username := "operator"
	password := "replacement"
	updated, err = store.patch(ctx, first.sourceID, rtspSourcePatch{username: &username, password: &password})
	if err != nil {
		t.Fatalf("patch(replace credentials) error = %v", err)
	}
	if updated.username != username || updated.password != password {
		t.Fatalf("replaced credentials = %q/%q", updated.username, updated.password)
	}
	updated, err = store.setDesiredState(ctx, first.sourceID, sourceDesiredStopped)
	if err != nil || updated.desiredState != sourceDesiredStopped {
		t.Fatalf("setDesiredState() = %+v, %v", updated, err)
	}
	updated, err = store.setDesiredState(ctx, first.sourceID, sourceDesiredRunning)
	if err != nil || updated.desiredState != sourceDesiredRunning {
		t.Fatalf("setDesiredState(running) = %+v, %v", updated, err)
	}
	if err := store.close(); err != nil {
		t.Fatalf("close() error = %v", err)
	}

	store, err = openSourceStore(ctx, path)
	if err != nil {
		t.Fatalf("reopen source store error = %v", err)
	}
	t.Cleanup(func() { _ = store.close() })
	persisted, err := store.get(ctx, first.sourceID)
	if err != nil {
		t.Fatalf("get(persisted) error = %v", err)
	}
	if persisted.streamName != first.streamName || persisted.url != updatedURL || persisted.username != username ||
		persisted.password != password || persisted.desiredState != sourceDesiredRunning {
		t.Fatalf("persisted source = %+v", persisted)
	}
	if err := store.deleteStopped(ctx, first.sourceID); !errors.Is(err, errSourceConflict) {
		t.Fatalf("delete running source error = %v", err)
	}

	conflictingName := second.streamName
	if _, err := store.patch(ctx, first.sourceID, rtspSourcePatch{streamName: &conflictingName}); !errors.Is(err, errSourceConflict) {
		t.Fatalf("patch(conflict) error = %v", err)
	}
	unchanged, err := store.get(ctx, first.sourceID)
	if err != nil || unchanged.streamName != first.streamName {
		t.Fatalf("source after conflict = %+v, %v", unchanged, err)
	}
	if _, err := store.setDesiredState(ctx, first.sourceID, sourceDesiredStopped); err != nil {
		t.Fatalf("stop source before delete error = %v", err)
	}
	if err := store.deleteStopped(ctx, first.sourceID); err != nil {
		t.Fatalf("delete() error = %v", err)
	}
	if _, err := store.get(ctx, first.sourceID); !errors.Is(err, errSourceNotFound) {
		t.Fatalf("get(deleted) error = %v", err)
	}
	if err := store.deleteStopped(ctx, first.sourceID); !errors.Is(err, errSourceNotFound) {
		t.Fatalf("delete(missing) error = %v", err)
	}
}

func TestSourceStoreRejectsInvalidAndDuplicateSources(t *testing.T) {
	ctx := context.Background()
	store := newTestSourceStore(t)
	source := rtspSource{
		sourceID: uuid.NewString(), streamName: "live/camera", url: "rtsp://camera.example/live",
		desiredState: sourceDesiredStopped,
	}
	if err := store.create(ctx, source); err != nil {
		t.Fatalf("create() error = %v", err)
	}
	duplicateName := source
	duplicateName.sourceID = uuid.NewString()
	if err := store.create(ctx, duplicateName); !errors.Is(err, errSourceConflict) {
		t.Fatalf("duplicate stream name error = %v", err)
	}
	duplicateID := source
	duplicateID.streamName = "live/other"
	if err := store.create(ctx, duplicateID); !errors.Is(err, errSourceConflict) {
		t.Fatalf("duplicate source id error = %v", err)
	}
	invalid := source
	invalid.sourceID = "not-a-uuid"
	if err := store.create(ctx, invalid); !errors.Is(err, errInvalidSource) {
		t.Fatalf("invalid source error = %v", err)
	}
	password := "secret"
	if _, err := store.patch(ctx, source.sourceID, rtspSourcePatch{password: &password}); !errors.Is(err, errInvalidSource) {
		t.Fatalf("password without username error = %v", err)
	}
}

func TestSourceStoreCreatesPrivateDatabaseFile(t *testing.T) {
	path := filepath.Join(t.TempDir(), "signaling.db")
	if err := os.WriteFile(path, nil, 0o644); err != nil {
		t.Fatalf("WriteFile() error = %v", err)
	}
	store, err := openSourceStore(context.Background(), path)
	if err != nil {
		t.Fatalf("openSourceStore() error = %v", err)
	}
	defer store.close()
	info, err := os.Stat(path)
	if err != nil {
		t.Fatalf("Stat() error = %v", err)
	}
	if permissions := info.Mode().Perm(); permissions != 0o600 {
		t.Fatalf("database permissions = %o", permissions)
	}
}

func TestSourceStoreListsEmptyCollection(t *testing.T) {
	sources, err := newTestSourceStore(t).list(context.Background())
	if err != nil {
		t.Fatalf("list() error = %v", err)
	}
	if sources == nil || len(sources) != 0 {
		t.Fatalf("list() = %#v", sources)
	}
}
