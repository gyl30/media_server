package main

import (
	"sync"
	"time"

	"github.com/emiago/sipgo/sip"
)

type registeredDevice struct {
	id             string
	contact        sip.Uri
	remoteEndpoint string
	expiresAt      time.Time
	lastHeartbeat  time.Time
	online         bool
}

type deviceRegistry struct {
	mu      sync.RWMutex
	devices map[string]registeredDevice
}

func newDeviceRegistry() *deviceRegistry {
	return &deviceRegistry{devices: make(map[string]registeredDevice)}
}

func (r *deviceRegistry) register(device registeredDevice) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	becameOnline := true
	if existing, ok := r.devices[device.id]; ok {
		becameOnline = !existing.online
	}
	device.contact = *device.contact.Clone()
	r.devices[device.id] = device
	return becameOnline
}

func (r *deviceRegistry) registeredSource(deviceID, remoteEndpoint string, now time.Time) bool {
	r.mu.RLock()
	defer r.mu.RUnlock()
	device, ok := r.devices[deviceID]
	return ok && device.online && now.Before(device.expiresAt) && device.remoteEndpoint == remoteEndpoint
}

func (r *deviceRegistry) unregister(deviceID string) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	_, existed := r.devices[deviceID]
	delete(r.devices, deviceID)
	return existed
}

func (r *deviceRegistry) get(deviceID string) (registeredDevice, bool) {
	r.mu.RLock()
	defer r.mu.RUnlock()
	device, ok := r.devices[deviceID]
	if ok {
		device.contact = *device.contact.Clone()
	}
	return device, ok
}

func (r *deviceRegistry) getOnline(deviceID string, now time.Time) (registeredDevice, bool) {
	device, ok := r.get(deviceID)
	return device, ok && device.online && now.Before(device.expiresAt)
}

func (r *deviceRegistry) len() int {
	r.mu.RLock()
	defer r.mu.RUnlock()
	return len(r.devices)
}

func (r *deviceRegistry) keepalive(deviceID, remoteEndpoint string, now time.Time) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	device, ok := r.devices[deviceID]
	if !ok || device.remoteEndpoint != remoteEndpoint || !now.Before(device.expiresAt) {
		return false
	}
	device.lastHeartbeat = now
	device.online = true
	r.devices[deviceID] = device
	return true
}

func (r *deviceRegistry) expire(now time.Time, heartbeatTimeout time.Duration) []string {
	r.mu.Lock()
	defer r.mu.Unlock()
	var offline []string
	for deviceID, device := range r.devices {
		if !now.Before(device.expiresAt) {
			if device.online {
				offline = append(offline, deviceID)
			}
			delete(r.devices, deviceID)
			continue
		}
		if device.online && !now.Before(device.lastHeartbeat.Add(heartbeatTimeout)) {
			device.online = false
			r.devices[deviceID] = device
			offline = append(offline, deviceID)
		}
	}
	return offline
}
