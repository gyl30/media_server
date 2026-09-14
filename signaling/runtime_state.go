package main

import (
	"errors"
	"sort"
	"sync"
)

var errRuntimeConflict = errors.New("runtime state conflict")

type observedRuntime struct {
	Type       string `json:"type"`
	ServerID   string `json:"server_id"`
	InstanceID string `json:"instance_id"`
	StreamID   string `json:"stream_id"`
	StreamName string `json:"stream_name"`
	SourceID   string `json:"source_id,omitempty"`
	Direction  string `json:"direction"`
	Protocol   string `json:"protocol"`
	State      string `json:"state"`
	Stage      string `json:"stage,omitempty"`
	EndReason  string `json:"end_reason,omitempty"`
	Error      string `json:"error,omitempty"`
}

type observedRuntimeRegistry struct {
	mu              sync.RWMutex
	byStreamID      map[string]observedRuntime
	currentBySource map[string]string
	onChange        func(observedRuntime)
}

func newObservedRuntimeRegistry() *observedRuntimeRegistry {
	return &observedRuntimeRegistry{
		byStreamID: make(map[string]observedRuntime), currentBySource: make(map[string]string),
	}
}

func (r *observedRuntimeRegistry) setOnChange(onChange func(observedRuntime)) {
	r.mu.Lock()
	r.onChange = onChange
	r.mu.Unlock()
}

func (r *observedRuntimeRegistry) apply(event observedRuntime) (bool, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.applyLocked(event, false)
}

func (r *observedRuntimeRegistry) acknowledgeSourceStopped(
	server mediaServerInstance,
	streamID, streamName, sourceID, protocol string,
) (bool, error) {
	event := observedRuntime{
		Type: "source_stopped", ServerID: server.serverID, InstanceID: server.instanceID,
		StreamID: streamID, StreamName: streamName, SourceID: sourceID,
		Direction: "input", Protocol: protocol, State: "stopped", EndReason: "requested",
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.applyLocked(event, true)
}

func (r *observedRuntimeRegistry) applyLocked(event observedRuntime, acceptExistingStopped bool) (bool, error) {
	current, exists := r.byStreamID[event.StreamID]
	if exists {
		if !sameRuntimeIdentity(current, event) {
			return false, errRuntimeConflict
		}
		if current.State == "stopped" {
			if current == event || acceptExistingStopped {
				return false, nil
			}
			return false, errRuntimeConflict
		}
		if current.State == "streaming" && event.State == "starting" {
			return false, errRuntimeConflict
		}
		if !validRuntimeTransition(current, event) {
			return false, errRuntimeConflict
		}
		if current == event {
			return false, nil
		}
	}
	r.byStreamID[event.StreamID] = event
	if event.SourceID != "" {
		if _, bound := r.currentBySource[event.SourceID]; !bound {
			r.currentBySource[event.SourceID] = event.StreamID
		}
	}
	if r.onChange != nil {
		r.onChange(event)
	}
	return true, nil
}

func (r *observedRuntimeRegistry) bindSource(sourceID, streamID string) (string, bool) {
	r.mu.Lock()
	previous, existed := r.currentBySource[sourceID]
	r.currentBySource[sourceID] = streamID
	r.mu.Unlock()
	return previous, existed
}

func (r *observedRuntimeRegistry) restoreSourceBinding(sourceID, expected, previous string, hadPrevious bool) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.currentBySource[sourceID] != expected {
		return
	}
	if hadPrevious {
		r.currentBySource[sourceID] = previous
		return
	}
	delete(r.currentBySource, sourceID)
}

func (r *observedRuntimeRegistry) currentForSource(sourceID string) (observedRuntime, bool) {
	r.mu.RLock()
	streamID, ok := r.currentBySource[sourceID]
	runtime, exists := r.byStreamID[streamID]
	r.mu.RUnlock()
	return runtime, ok && exists
}

func (r *observedRuntimeRegistry) snapshot() []observedRuntime {
	r.mu.RLock()
	runtimes := make([]observedRuntime, 0, len(r.byStreamID))
	for _, runtime := range r.byStreamID {
		runtimes = append(runtimes, runtime)
	}
	r.mu.RUnlock()
	sort.Slice(runtimes, func(left, right int) bool {
		return runtimes[left].StreamID < runtimes[right].StreamID
	})
	return runtimes
}

func (r *observedRuntimeRegistry) mediaServerOffline(serverID, instanceID string) []observedRuntime {
	r.mu.Lock()
	defer r.mu.Unlock()
	var changed []observedRuntime
	for streamID, runtime := range r.byStreamID {
		if runtime.ServerID != serverID || runtime.InstanceID != instanceID || runtime.State == "stopped" {
			continue
		}
		runtime.Type = "runtime_error"
		runtime.State = "stopped"
		runtime.EndReason = "runtime_error"
		runtime.Error = "media_server_offline"
		r.byStreamID[streamID] = runtime
		changed = append(changed, runtime)
		if r.onChange != nil {
			r.onChange(runtime)
		}
	}
	return changed
}

func sameRuntimeIdentity(left, right observedRuntime) bool {
	return left.ServerID == right.ServerID && left.InstanceID == right.InstanceID && left.StreamID == right.StreamID &&
		left.StreamName == right.StreamName && left.SourceID == right.SourceID && left.Direction == right.Direction &&
		left.Protocol == right.Protocol
}

func validRuntimeTransition(current, next observedRuntime) bool {
	if next.State != "stopped" {
		return next.Type == current.Type
	}
	if next.Type == "protocol_error" || next.Type == "runtime_error" {
		return true
	}
	switch current.Type {
	case "source_started":
		return next.Type == "source_stopped"
	case "publisher_connected":
		return next.Type == "publisher_disconnected"
	case "output_started":
		return next.Type == "output_stopped"
	default:
		return false
	}
}
