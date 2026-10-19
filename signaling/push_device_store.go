package main

import (
	"context"
	"database/sql"
	"errors"
	"strings"
	"time"
)

// 推流协议只决定界面展示哪一种接入地址；发布凭据本身不绑定协议。
type pushProtocol string

const (
	pushProtocolRTMP pushProtocol = "rtmp"
	pushProtocolRTSP pushProtocol = "rtsp"
	pushProtocolWHIP pushProtocol = "whip"
)

// 推流接入凭据（即接入地址中的 token）从确认添加或重新生成时起 5 分钟内可首次使用；播放 token 的有效期另行配置。
const pushCredentialTTL = 5 * time.Minute

// 数据库以毫秒保存时间，截止时间在生成时即按毫秒截断，返回值与重新读取的一致。
func pushCredentialDeadline(now time.Time) time.Time {
	return now.Add(pushCredentialTTL).Truncate(time.Millisecond)
}

func validPushProtocol(protocol pushProtocol) bool {
	switch protocol {
	case pushProtocolRTMP, pushProtocolRTSP, pushProtocolWHIP:
		return true
	}
	return false
}

// usedAt 非零表示凭据已被一次发布认证消费。
type pushDevice struct {
	deviceID   string
	name       string
	protocol   pushProtocol
	credential string
	expiresAt  time.Time
	usedAt     time.Time
}

func unixMilli(value time.Time) int64 {
	if value.IsZero() {
		return 0
	}
	return value.UnixMilli()
}

func fromUnixMilli(value int64) time.Time {
	if value == 0 {
		return time.Time{}
	}
	return time.UnixMilli(value)
}

const pushDeviceColumns = `device_id, name, protocol, credential, credential_expires_at, credential_used_at`

func scanPushDevice(scan func(...any) error) (pushDevice, error) {
	var device pushDevice
	var expiresAt, usedAt int64
	if err := scan(&device.deviceID, &device.name, &device.protocol, &device.credential, &expiresAt, &usedAt); err != nil {
		return pushDevice{}, err
	}
	device.expiresAt, device.usedAt = fromUnixMilli(expiresAt), fromUnixMilli(usedAt)
	return device, nil
}

func (s *sourceStore) createPushDevice(ctx context.Context, device pushDevice) error {
	if !validUUIDv4(device.deviceID) || strings.TrimSpace(device.name) == "" || !validPushProtocol(device.protocol) || !validStreamToken(device.credential) {
		return errInvalidDevice
	}
	_, err := s.db.ExecContext(ctx, `INSERT INTO push_devices (device_id, name, protocol, credential, credential_expires_at) VALUES (?, ?, ?, ?, ?)`,
		device.deviceID, device.name, device.protocol, device.credential, unixMilli(device.expiresAt))
	return err
}

func (s *sourceStore) getPushDevice(ctx context.Context, deviceID string) (pushDevice, error) {
	device, err := scanPushDevice(s.db.QueryRowContext(ctx, `SELECT `+pushDeviceColumns+` FROM push_devices WHERE device_id = ?`, deviceID).Scan)
	if errors.Is(err, sql.ErrNoRows) {
		return pushDevice{}, errDeviceNotFound
	}
	return device, err
}

func (s *sourceStore) listPushDevices(ctx context.Context) ([]pushDevice, error) {
	rows, err := s.db.QueryContext(ctx, `SELECT `+pushDeviceColumns+` FROM push_devices ORDER BY device_id`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	devices := make([]pushDevice, 0)
	for rows.Next() {
		device, err := scanPushDevice(rows.Scan)
		if err != nil {
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

// 用新凭据替换旧凭据：旧凭据立即失效，新凭据未使用。
func (s *sourceStore) renewPushCredential(ctx context.Context, deviceID, credential string, expiresAt time.Time) error {
	result, err := s.db.ExecContext(ctx, `UPDATE push_devices SET credential = ?, credential_expires_at = ?, credential_used_at = 0 WHERE device_id = ?`,
		credential, unixMilli(expiresAt), deviceID)
	if err != nil {
		return err
	}
	changed, err := result.RowsAffected()
	if err == nil && changed == 0 {
		return errDeviceNotFound
	}
	return err
}

// 原子消费未使用、未过期的凭据：并发认证只有一次成功，消费事实随数据库持久化。
// 返回凭据所属设备；凭据不存在、已使用或已过期时返回空。
func (s *sourceStore) consumePushCredential(ctx context.Context, credential string, now time.Time) (string, error) {
	var deviceID string
	err := s.db.QueryRowContext(ctx, `UPDATE push_devices SET credential_used_at = ?
		WHERE credential = ? AND credential_used_at = 0 AND credential_expires_at > ? RETURNING device_id`,
		unixMilli(now), credential, unixMilli(now)).Scan(&deviceID)
	if errors.Is(err, sql.ErrNoRows) {
		return "", nil
	}
	if err != nil {
		return "", err
	}
	return deviceID, nil
}
