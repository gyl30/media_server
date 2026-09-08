package main

import (
	"errors"
	"sync"
	"time"
)

var (
	errMediaServerConflict = errors.New("media server instance conflict")
	errMediaServerStale    = errors.New("media server instance is stale")
)

type mediaServerRegistration struct {
	ServerID   string `json:"server_id"`
	InstanceID string `json:"instance_id"`
	ControlURL string `json:"control_url"`
	MediaIP    string `json:"media_ip"`
}

type mediaServerHeartbeat struct {
	ServerID   string `json:"server_id"`
	InstanceID string `json:"instance_id"`
}

type mediaServerKey struct {
	serverID   string
	instanceID string
}

type mediaServerInstance struct {
	serverID      string
	instanceID    string
	controlURL    string
	mediaIP       string
	lastHeartbeat time.Time
	online        bool
}

type mediaServerRegistry struct {
	mu        sync.RWMutex
	instances map[mediaServerKey]mediaServerInstance
	current   map[string]mediaServerKey
	online    []mediaServerKey
}

func newMediaServerRegistry() *mediaServerRegistry {
	return &mediaServerRegistry{
		instances: make(map[mediaServerKey]mediaServerInstance),
		current:   make(map[string]mediaServerKey),
	}
}

func (r *mediaServerRegistry) register(registration mediaServerRegistration, now time.Time) error {
	key := mediaServerKey{serverID: registration.ServerID, instanceID: registration.InstanceID}
	r.mu.Lock()
	defer r.mu.Unlock()
	if _, exists := r.instances[key]; exists {
		return errMediaServerConflict
	}
	if currentKey, exists := r.current[registration.ServerID]; exists && r.instances[currentKey].online {
		return errMediaServerConflict
	}
	instance := mediaServerInstance{
		serverID:      registration.ServerID,
		instanceID:    registration.InstanceID,
		controlURL:    registration.ControlURL,
		mediaIP:       registration.MediaIP,
		lastHeartbeat: now,
		online:        true,
	}
	r.instances[key] = instance
	r.current[registration.ServerID] = key
	r.online = append(r.online, key)
	return nil
}

func (r *mediaServerRegistry) heartbeat(serverID, instanceID string, now time.Time) error {
	key := mediaServerKey{serverID: serverID, instanceID: instanceID}
	r.mu.Lock()
	defer r.mu.Unlock()
	instance, exists := r.instances[key]
	if !exists || !instance.online {
		return errMediaServerStale
	}
	instance.lastHeartbeat = now
	r.instances[key] = instance
	return nil
}

func (r *mediaServerRegistry) expire(now time.Time, timeout time.Duration) []mediaServerInstance {
	r.mu.Lock()
	defer r.mu.Unlock()
	var offline []mediaServerInstance
	active := r.online[:0]
	for _, key := range r.online {
		instance := r.instances[key]
		if !now.Before(instance.lastHeartbeat.Add(timeout)) {
			instance.online = false
			r.instances[key] = instance
			offline = append(offline, instance)
			continue
		}
		active = append(active, key)
	}
	r.online = active
	return offline
}

func (r *mediaServerRegistry) selectOnline() (mediaServerInstance, bool) {
	r.mu.RLock()
	defer r.mu.RUnlock()
	if len(r.online) == 0 {
		return mediaServerInstance{}, false
	}
	return r.instances[r.online[0]], true
}

func (r *mediaServerRegistry) isOnline(server mediaServerInstance) bool {
	r.mu.RLock()
	defer r.mu.RUnlock()
	instance, ok := r.instances[mediaServerKey{serverID: server.serverID, instanceID: server.instanceID}]
	return ok && instance.online
}

func (r *mediaServerRegistry) onlineCount() int {
	r.mu.RLock()
	defer r.mu.RUnlock()
	return len(r.online)
}
