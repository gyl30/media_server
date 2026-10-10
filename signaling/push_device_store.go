package main

import (
	"context"
	"database/sql"
	"errors"
	"strings"
)

type pushDevice struct {
	deviceID string
	name     string
}

func (s *sourceStore) createPushDevice(ctx context.Context, device pushDevice) error {
	if !validUUIDv4(device.deviceID) || strings.TrimSpace(device.name) == "" {
		return errInvalidDevice
	}
	_, err := s.db.ExecContext(ctx, `INSERT INTO push_devices (device_id, name) VALUES (?, ?)`, device.deviceID, device.name)
	return err
}

func (s *sourceStore) getPushDevice(ctx context.Context, deviceID string) (pushDevice, error) {
	var device pushDevice
	err := s.db.QueryRowContext(ctx, `SELECT device_id, name FROM push_devices WHERE device_id = ?`, deviceID).Scan(&device.deviceID, &device.name)
	if errors.Is(err, sql.ErrNoRows) {
		return pushDevice{}, errDeviceNotFound
	}
	return device, err
}

func (s *sourceStore) listPushDevices(ctx context.Context) ([]pushDevice, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT device_id, name FROM push_devices ORDER BY device_id`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	devices := make([]pushDevice, 0)
	for rows.Next() {
		var device pushDevice
		if err := rows.Scan(&device.deviceID, &device.name); err != nil {
			return nil, err
		}
		devices = append(devices, device)
	}
	return devices, rows.Err()
}

func (s *sourceStore) patchPushDevice(ctx context.Context, deviceID, name string) error {
	if strings.TrimSpace(name) == "" {
		return errInvalidDevice
	}
	result, err := s.db.ExecContext(ctx, `UPDATE push_devices SET name = ? WHERE device_id = ?`, name, deviceID)
	if err != nil {
		return err
	}
	changed, err := result.RowsAffected()
	if err == nil && changed == 0 {
		return errDeviceNotFound
	}
	return err
}
