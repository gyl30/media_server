package main

import (
	"errors"
	"sort"
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
	RTMPPort   uint16 `json:"rtmp_port"`
	RTSPPort   uint16 `json:"rtsp_port"`
	HTTPPort   uint16 `json:"http_port"`
}

type mediaServerHeartbeat struct {
	ServerID   string `json:"server_id"`
	InstanceID string `json:"instance_id"`
}

type mediaServerInstance struct {
	serverID      string
	instanceID    string
	controlURL    string
	mediaIP       string
	rtmpPort      uint16
	rtspPort      uint16
	httpPort      uint16
	lastHeartbeat time.Time
	online        bool
}

type mediaServerRegistry struct {
	mu        sync.RWMutex
	instances map[string]mediaServerInstance
}

func newMediaServerRegistry() *mediaServerRegistry {
	return &mediaServerRegistry{
		instances: make(map[string]mediaServerInstance),
	}
}

func (r *mediaServerRegistry) register(registration mediaServerRegistration, now time.Time) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	current, exists := r.instances[registration.ServerID]
	if exists {
		if current.instanceID == registration.InstanceID || current.online {
			return errMediaServerConflict
		}
	}
	r.instances[registration.ServerID] = mediaServerInstance{
		serverID:      registration.ServerID,
		instanceID:    registration.InstanceID,
		controlURL:    registration.ControlURL,
		mediaIP:       registration.MediaIP,
		rtmpPort:      registration.RTMPPort,
		rtspPort:      registration.RTSPPort,
		httpPort:      registration.HTTPPort,
		lastHeartbeat: now,
		online:        true,
	}
	return nil
}

func (r *mediaServerRegistry) heartbeat(serverID, instanceID string, now time.Time) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	instance, exists := r.instances[serverID]
	if !exists || instance.instanceID != instanceID || !instance.online {
		return errMediaServerStale
	}
	instance.lastHeartbeat = now
	r.instances[serverID] = instance
	return nil
}

func (r *mediaServerRegistry) expire(now time.Time, timeout time.Duration) []mediaServerInstance {
	r.mu.Lock()
	defer r.mu.Unlock()
	var offline []mediaServerInstance
	for serverID, instance := range r.instances {
		if !instance.online {
			continue
		}
		if !now.Before(instance.lastHeartbeat.Add(timeout)) {
			instance.online = false
			r.instances[serverID] = instance
			offline = append(offline, instance)
		}
	}
	return offline
}

func (r *mediaServerRegistry) selectOnline() (mediaServerInstance, bool) {
	r.mu.RLock()
	defer r.mu.RUnlock()
	var selected mediaServerInstance
	found := false
	for serverID, instance := range r.instances {
		if !instance.online || (found && serverID >= selected.serverID) {
			continue
		}
		selected = instance
		found = true
	}
	return selected, found
}

func (r *mediaServerRegistry) isOnline(server mediaServerInstance) bool {
	r.mu.RLock()
	defer r.mu.RUnlock()
	instance, ok := r.instances[server.serverID]
	return ok && instance.instanceID == server.instanceID && instance.online
}

func (r *mediaServerRegistry) withOnlineInstance(serverID, instanceID string, operation func()) bool {
	r.mu.RLock()
	defer r.mu.RUnlock()
	instance, ok := r.instances[serverID]
	if !ok || instance.instanceID != instanceID || !instance.online {
		return false
	}
	operation()
	return true
}

func (r *mediaServerRegistry) currentInstances() []mediaServerInstance {
	r.mu.RLock()
	instances := make([]mediaServerInstance, 0, len(r.instances))
	for _, instance := range r.instances {
		instances = append(instances, instance)
	}
	r.mu.RUnlock()
	sort.Slice(instances, func(left, right int) bool {
		return instances[left].serverID < instances[right].serverID
	})
	return instances
}
