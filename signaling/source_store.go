package main

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"os"

	_ "modernc.org/sqlite"
)

var (
	errSourceNotFound = errors.New("source not found")
	errSourceConflict = errors.New("source conflict")
	errInvalidSource  = errors.New("invalid source")
)

type sourceDesiredState string

const (
	sourceDesiredStopped sourceDesiredState = "stopped"
	sourceDesiredRunning sourceDesiredState = "running"
)

type rtspSource struct {
	sourceID     string
	streamName   string
	url          string
	username     string
	password     string
	desiredState sourceDesiredState
}

type rtspSourcePatch struct {
	streamName *string
	url        *string
	username   *string
	password   *string
}

type sourceStore struct {
	db *sql.DB
}

func openSourceStore(ctx context.Context, path string) (*sourceStore, error) {
	if path == "" {
		return nil, fmt.Errorf("database path is empty")
	}
	file, err := os.OpenFile(path, os.O_RDWR|os.O_CREATE, 0o600)
	if err != nil {
		return nil, fmt.Errorf("open database file: %w", err)
	}
	if err := file.Chmod(0o600); err != nil {
		_ = file.Close()
		return nil, fmt.Errorf("set database permissions: %w", err)
	}
	if err := file.Close(); err != nil {
		return nil, fmt.Errorf("close database file: %w", err)
	}

	db, err := sql.Open("sqlite", path)
	if err != nil {
		return nil, fmt.Errorf("open database: %w", err)
	}
	db.SetMaxOpenConns(1)
	db.SetMaxIdleConns(1)
	store := &sourceStore{db: db}
	if err := db.PingContext(ctx); err != nil {
		_ = db.Close()
		return nil, fmt.Errorf("connect database: %w", err)
	}
	if _, err := db.ExecContext(ctx, `
		CREATE TABLE IF NOT EXISTS rtsp_sources (
			source_id TEXT PRIMARY KEY,
			stream_name TEXT NOT NULL UNIQUE,
			url TEXT NOT NULL,
			username TEXT NOT NULL,
			password TEXT NOT NULL,
			desired_state TEXT NOT NULL CHECK (desired_state IN ('stopped', 'running'))
		)`); err != nil {
		_ = db.Close()
		return nil, fmt.Errorf("initialize database: %w", err)
	}
	return store, nil
}

func (s *sourceStore) close() error {
	return s.db.Close()
}

func (s *sourceStore) create(ctx context.Context, source rtspSource) error {
	if !validRTSPSource(source) {
		return errInvalidSource
	}
	result, err := s.db.ExecContext(ctx, `
		INSERT INTO rtsp_sources (source_id, stream_name, url, username, password, desired_state)
		VALUES (?, ?, ?, ?, ?, ?)
		ON CONFLICT DO NOTHING`,
		source.sourceID, source.streamName, source.url, source.username, source.password, source.desiredState)
	if err != nil {
		return fmt.Errorf("create source: %w", err)
	}
	changed, err := result.RowsAffected()
	if err != nil {
		return fmt.Errorf("create source result: %w", err)
	}
	if changed == 0 {
		return errSourceConflict
	}
	return nil
}

func (s *sourceStore) get(ctx context.Context, sourceID string) (rtspSource, error) {
	return scanRTSPSource(s.db.QueryRowContext(ctx, `
		SELECT source_id, stream_name, url, username, password, desired_state
		FROM rtsp_sources WHERE source_id = ?`, sourceID))
}

func (s *sourceStore) list(ctx context.Context) ([]rtspSource, error) {
	rows, err := s.db.QueryContext(ctx, `
		SELECT source_id, stream_name, url, username, password, desired_state
		FROM rtsp_sources ORDER BY stream_name, source_id`)
	if err != nil {
		return nil, fmt.Errorf("list sources: %w", err)
	}
	defer rows.Close()

	sources := make([]rtspSource, 0)
	for rows.Next() {
		source, err := scanRTSPSource(rows)
		if err != nil {
			return nil, err
		}
		sources = append(sources, source)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("list sources: %w", err)
	}
	return sources, nil
}

func (s *sourceStore) patch(ctx context.Context, sourceID string, patch rtspSourcePatch) (rtspSource, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return rtspSource{}, fmt.Errorf("begin source update: %w", err)
	}
	defer tx.Rollback()

	source, err := scanRTSPSource(tx.QueryRowContext(ctx, `
		SELECT source_id, stream_name, url, username, password, desired_state
		FROM rtsp_sources WHERE source_id = ?`, sourceID))
	if err != nil {
		return rtspSource{}, err
	}
	if patch.streamName != nil {
		source.streamName = *patch.streamName
	}
	if patch.url != nil {
		source.url = *patch.url
	}
	if patch.username != nil {
		source.username = *patch.username
	}
	if patch.password != nil {
		source.password = *patch.password
	}
	if !validRTSPSource(source) {
		return rtspSource{}, errInvalidSource
	}

	var conflictingID string
	err = tx.QueryRowContext(ctx, `
		SELECT source_id FROM rtsp_sources WHERE stream_name = ? AND source_id <> ?`, source.streamName, source.sourceID).Scan(&conflictingID)
	if err == nil {
		return rtspSource{}, errSourceConflict
	}
	if !errors.Is(err, sql.ErrNoRows) {
		return rtspSource{}, fmt.Errorf("check source conflict: %w", err)
	}
	if _, err := tx.ExecContext(ctx, `
		UPDATE rtsp_sources SET stream_name = ?, url = ?, username = ?, password = ? WHERE source_id = ?`,
		source.streamName, source.url, source.username, source.password, source.sourceID); err != nil {
		return rtspSource{}, fmt.Errorf("update source: %w", err)
	}
	if err := tx.Commit(); err != nil {
		return rtspSource{}, fmt.Errorf("commit source update: %w", err)
	}
	return source, nil
}

func (s *sourceStore) deleteStopped(ctx context.Context, sourceID string) error {
	result, err := s.db.ExecContext(ctx, `DELETE FROM rtsp_sources WHERE source_id = ? AND desired_state = ?`, sourceID, sourceDesiredStopped)
	if err != nil {
		return fmt.Errorf("delete source: %w", err)
	}
	changed, err := result.RowsAffected()
	if err != nil {
		return fmt.Errorf("delete source result: %w", err)
	}
	if changed == 0 {
		if _, err := s.get(ctx, sourceID); errors.Is(err, errSourceNotFound) {
			return errSourceNotFound
		} else if err != nil {
			return err
		}
		return errSourceConflict
	}
	return nil
}

func (s *sourceStore) setDesiredState(ctx context.Context, sourceID string, desired sourceDesiredState) (rtspSource, error) {
	if desired != sourceDesiredStopped && desired != sourceDesiredRunning {
		return rtspSource{}, errInvalidSource
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return rtspSource{}, fmt.Errorf("begin desired state update: %w", err)
	}
	defer tx.Rollback()
	result, err := tx.ExecContext(ctx, `UPDATE rtsp_sources SET desired_state = ? WHERE source_id = ?`, desired, sourceID)
	if err != nil {
		return rtspSource{}, fmt.Errorf("update desired state: %w", err)
	}
	changed, err := result.RowsAffected()
	if err != nil {
		return rtspSource{}, fmt.Errorf("desired state result: %w", err)
	}
	if changed == 0 {
		return rtspSource{}, errSourceNotFound
	}
	source, err := scanRTSPSource(tx.QueryRowContext(ctx, `
		SELECT source_id, stream_name, url, username, password, desired_state
		FROM rtsp_sources WHERE source_id = ?`, sourceID))
	if err != nil {
		return rtspSource{}, err
	}
	if err := tx.Commit(); err != nil {
		return rtspSource{}, fmt.Errorf("commit desired state update: %w", err)
	}
	return source, nil
}

type rowScanner interface {
	Scan(...any) error
}

func scanRTSPSource(row rowScanner) (rtspSource, error) {
	var source rtspSource
	if err := row.Scan(
		&source.sourceID,
		&source.streamName,
		&source.url,
		&source.username,
		&source.password,
		&source.desiredState,
	); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return rtspSource{}, errSourceNotFound
		}
		return rtspSource{}, fmt.Errorf("read source: %w", err)
	}
	return source, nil
}

func validRTSPSource(source rtspSource) bool {
	return validUUIDv4(source.sourceID) && source.streamName != "" && validRTSPSourceURL(source.url) &&
		(source.password == "" || source.username != "") &&
		(source.desiredState == sourceDesiredStopped || source.desiredState == sourceDesiredRunning)
}
