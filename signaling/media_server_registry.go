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
	serverID          string
	instanceID        string
	controlURL        string
	mediaIP           string
	lastHeartbeat     time.Time
	online            bool
	registrationOrder uint64
}

type mediaServerRegistry struct {
	mu        sync.RWMutex
	instances map[mediaServerKey]mediaServerInstance
	current   map[string]mediaServerKey
	order     []mediaServerKey
	nextOrder uint64
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
	r.nextOrder++
	instance := mediaServerInstance{
		serverID:          registration.ServerID,
		instanceID:        registration.InstanceID,
		controlURL:        registration.ControlURL,
		mediaIP:           registration.MediaIP,
		lastHeartbeat:     now,
		online:            true,
		registrationOrder: r.nextOrder,
	}
	r.instances[key] = instance
	r.current[registration.ServerID] = key
	r.order = append(r.order, key)
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
	for key, instance := range r.instances {
		if instance.online && !now.Before(instance.lastHeartbeat.Add(timeout)) {
			instance.online = false
			r.instances[key] = instance
			offline = append(offline, instance)
		}
	}
	return offline
}

func (r *mediaServerRegistry) selectOnline() (mediaServerInstance, bool) {
	r.mu.RLock()
	defer r.mu.RUnlock()
	for _, key := range r.order {
		instance := r.instances[key]
		if instance.online {
			return instance, true
		}
	}
	return mediaServerInstance{}, false
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
	count := 0
	for _, instance := range r.instances {
		if instance.online {
			count++
		}
	}
	return count
}
