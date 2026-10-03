package main

import (
	"context"
	"database/sql"
	"errors"
	"strings"
)

var (
	errDeviceNotFound = errors.New("device not found")
	errDeviceExists   = errors.New("device exists")
	errInvalidDevice  = errors.New("invalid device")
)

type gbDevice struct {
	deviceID string
	name     string
}

type deviceStore struct {
	db *sql.DB
}

func newDeviceStore(ctx context.Context, db *sql.DB) (*deviceStore, error) {
	_, err := db.ExecContext(ctx, `CREATE TABLE IF NOT EXISTS gb_devices (
		device_id TEXT PRIMARY KEY,
		name TEXT NOT NULL
	)`)
	if err != nil {
		return nil, err
	}
	return &deviceStore{db: db}, nil
}

func (s *deviceStore) create(ctx context.Context, device gbDevice) error {
	if !validDigits(device.deviceID, 20) || strings.TrimSpace(device.name) == "" {
		return errInvalidDevice
	}
	result, err := s.db.ExecContext(ctx, `INSERT INTO gb_devices (device_id, name) VALUES (?, ?) ON CONFLICT DO NOTHING`, device.deviceID, device.name)
	if err != nil {
		return err
	}
	changed, err := result.RowsAffected()
	if err != nil {
		return err
	}
	if changed == 0 {
		return errDeviceExists
	}
	return nil
}

func (s *deviceStore) get(ctx context.Context, deviceID string) (gbDevice, error) {
	var device gbDevice
	err := s.db.QueryRowContext(ctx, `SELECT device_id, name FROM gb_devices WHERE device_id = ?`, deviceID).Scan(&device.deviceID, &device.name)
	if errors.Is(err, sql.ErrNoRows) {
		return gbDevice{}, errDeviceNotFound
	}
	return device, err
}

func (s *deviceStore) list(ctx context.Context) ([]gbDevice, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT device_id, name FROM gb_devices ORDER BY device_id`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	devices := make([]gbDevice, 0)
	for rows.Next() {
		var device gbDevice
		if err := rows.Scan(&device.deviceID, &device.name); err != nil {
			return nil, err
		}
		devices = append(devices, device)
	}
	return devices, rows.Err()
}

func (s *deviceStore) exists(ctx context.Context, deviceID string) (bool, error) {
	_, err := s.get(ctx, deviceID)
	if errors.Is(err, errDeviceNotFound) {
		return false, nil
	}
	return err == nil, err
}

func (s *deviceStore) delete(ctx context.Context, deviceID string) error {
	result, err := s.db.ExecContext(ctx, `DELETE FROM gb_devices WHERE device_id = ?`, deviceID)
	if err != nil {
		return err
	}
	changed, err := result.RowsAffected()
	if err != nil {
		return err
	}
	if changed == 0 {
		return errDeviceNotFound
	}
	return nil
}
