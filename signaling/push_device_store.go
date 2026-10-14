package main

import (
	"context"
	"database/sql"
	"errors"
	"strings"
)

// pushProtocol 是推流设备选择的接入协议，仅用于展示与签发结果的取舍；空值表示未指定（旧数据）。
// 发布 token 本身不绑定协议。
type pushProtocol string

const (
	pushProtocolUnspecified pushProtocol = ""
	pushProtocolRTMP        pushProtocol = "rtmp"
	pushProtocolRTSP        pushProtocol = "rtsp"
	pushProtocolWHIP        pushProtocol = "whip"
)

func validPushProtocol(protocol pushProtocol) bool {
	switch protocol {
	case pushProtocolUnspecified, pushProtocolRTMP, pushProtocolRTSP, pushProtocolWHIP:
		return true
	}
	return false
}

type pushDevice struct {
	deviceID string
	name     string
	protocol pushProtocol
}

// 旧库的 push_devices 没有 protocol 列：按需补列，已有设备保持未指定。
func ensurePushProtocolColumn(ctx context.Context, db *sql.DB) error {
	var columns int
	if err := db.QueryRowContext(ctx, `SELECT COUNT(*) FROM pragma_table_info('push_devices') WHERE name = 'protocol'`).Scan(&columns); err != nil {
		return err
	}
	if columns != 0 {
		return nil
	}
	_, err := db.ExecContext(ctx, `ALTER TABLE push_devices ADD COLUMN protocol TEXT NOT NULL DEFAULT '' CHECK (protocol IN ('', 'rtmp', 'rtsp', 'whip'))`)
	return err
}

func (s *sourceStore) createPushDevice(ctx context.Context, device pushDevice) error {
	if !validUUIDv4(device.deviceID) || strings.TrimSpace(device.name) == "" || !validPushProtocol(device.protocol) {
		return errInvalidDevice
	}
	_, err := s.db.ExecContext(ctx, `INSERT INTO push_devices (device_id, name, protocol) VALUES (?, ?, ?)`, device.deviceID, device.name, device.protocol)
	return err
}

func (s *sourceStore) getPushDevice(ctx context.Context, deviceID string) (pushDevice, error) {
	var device pushDevice
	err := s.db.QueryRowContext(ctx, `SELECT device_id, name, protocol FROM push_devices WHERE device_id = ?`, deviceID).Scan(&device.deviceID, &device.name, &device.protocol)
	if errors.Is(err, sql.ErrNoRows) {
		return pushDevice{}, errDeviceNotFound
	}
	return device, err
}

func (s *sourceStore) listPushDevices(ctx context.Context) ([]pushDevice, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT device_id, name, protocol FROM push_devices ORDER BY device_id`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	devices := make([]pushDevice, 0)
	for rows.Next() {
		var device pushDevice
		if err := rows.Scan(&device.deviceID, &device.name, &device.protocol); err != nil {
			return nil, err
		}
		devices = append(devices, device)
	}
	return devices, rows.Err()
}

// protocol 为 nil 时保留原协议。
func (s *sourceStore) patchPushDevice(ctx context.Context, deviceID, name string, protocol *pushProtocol) error {
	if strings.TrimSpace(name) == "" || (protocol != nil && !validPushProtocol(*protocol)) {
		return errInvalidDevice
	}
	result, err := s.db.ExecContext(ctx, `UPDATE push_devices SET name = ?, protocol = COALESCE(?, protocol) WHERE device_id = ?`, name, protocol, deviceID)
	if err != nil {
		return err
	}
	changed, err := result.RowsAffected()
	if err == nil && changed == 0 {
		return errDeviceNotFound
	}
	return err
}
