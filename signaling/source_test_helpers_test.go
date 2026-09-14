package main

import (
	"context"
	"log/slog"
	"path/filepath"
	"testing"
)

func newTestSourceStore(t *testing.T) *sourceStore {
	t.Helper()
	store, err := openSourceStore(context.Background(), filepath.Join(t.TempDir(), "signaling.db"))
	if err != nil {
		t.Fatalf("openSourceStore() error = %v", err)
	}
	t.Cleanup(func() {
		if err := store.close(); err != nil {
			t.Errorf("sourceStore.close() error = %v", err)
		}
	})
	return store
}

func newTestInfrastructureServer(t *testing.T, cfg config, registry *mediaServerRegistry, logger *slog.Logger) *infrastructureServer {
	t.Helper()
	return newInfrastructureServer(cfg, registry, newTestSourceStore(t), logger)
}
