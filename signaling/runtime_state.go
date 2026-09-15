package main

import (
	"errors"
	"sort"
	"sync"
)

var errRuntimeConflict = errors.New("runtime state conflict")

const maxRecentStoppedRuntimes = 500

type observedRuntime struct {
	Kind       string `json:"kind"`
	ServerID   string `json:"server_id"`
	InstanceID string `json:"instance_id"`
	StreamID   string `json:"stream_id"`
	StreamName string `json:"stream_name"`
	SourceID   string `json:"source_id,omitempty"`
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
	recentStopped   []string
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
		Kind: "source", ServerID: server.serverID, InstanceID: server.instanceID,
		StreamID: streamID, StreamName: streamName, SourceID: sourceID,
		Protocol: protocol, State: "stopped", EndReason: "requested",
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.applyLocked(event, true)
}

func (r *observedRuntimeRegistry) applyLocked(event observedRuntime, acceptExistingStopped bool) (bool, error) {
	current, exists := r.byStreamID[event.StreamID]
	if event.SourceID != "" {
		currentStreamID, bound := r.currentBySource[event.SourceID]
		if !bound || (currentStreamID != event.StreamID && (!exists || event.State != "stopped")) {
			return false, errRuntimeConflict
		}
	}
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
		if current == event {
			return false, nil
		}
	} else if event.SourceID != "" {
		if currentStreamID, bound := r.currentBySource[event.SourceID]; bound && currentStreamID != event.StreamID {
			return false, errRuntimeConflict
		}
	}
	r.byStreamID[event.StreamID] = event
	if event.State == "stopped" {
		r.retainStoppedLocked(event.StreamID)
	}
	if r.onChange != nil {
		r.onChange(event)
	}
	return true, nil
}

func (r *observedRuntimeRegistry) bindSource(sourceID, streamID string) (string, bool, *observedRuntime) {
	r.mu.Lock()
	previous, existed := r.currentBySource[sourceID]
	var previousRuntime *observedRuntime
	if runtime, ok := r.byStreamID[previous]; ok {
		previousRuntime = &runtime
	}
	r.replaceSourceBindingLocked(sourceID, streamID)
	r.mu.Unlock()
	return previous, existed, previousRuntime
}

func (r *observedRuntimeRegistry) restoreSourceBinding(
	sourceID, expected, previous string, hadPrevious bool, previousRuntime *observedRuntime,
) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.currentBySource[sourceID] != expected {
		return
	}
	if runtime, observed := r.byStreamID[expected]; observed {
		if runtime.State != "stopped" || !hadPrevious {
			return
		}
	}
	if hadPrevious {
		if previousRuntime != nil {
			if _, exists := r.byStreamID[previous]; !exists {
				r.byStreamID[previous] = *previousRuntime
			}
		}
		r.replaceSourceBindingLocked(sourceID, previous)
		return
	}
	r.replaceSourceBindingLocked(sourceID, "")
}

func (r *observedRuntimeRegistry) sourceBinding(sourceID string) (string, bool) {
	r.mu.RLock()
	streamID, exists := r.currentBySource[sourceID]
	r.mu.RUnlock()
	return streamID, exists
}

func (r *observedRuntimeRegistry) unbindSource(sourceID, expectedStreamID string) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.currentBySource[sourceID] != expectedStreamID {
		return false
	}
	r.replaceSourceBindingLocked(sourceID, "")
	return true
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
		runtime.State = "stopped"
		runtime.EndReason = "runtime_error"
		runtime.Error = "media_server_offline"
		r.byStreamID[streamID] = runtime
		r.retainStoppedLocked(streamID)
		changed = append(changed, runtime)
		if r.onChange != nil {
			r.onChange(runtime)
		}
	}
	return changed
}

func (r *observedRuntimeRegistry) replaceSourceBindingLocked(sourceID, streamID string) {
	previous, hadPrevious := r.currentBySource[sourceID]
	if streamID == "" {
		delete(r.currentBySource, sourceID)
	} else {
		r.currentBySource[sourceID] = streamID
		r.removeRecentStoppedLocked(streamID)
	}
	if hadPrevious && previous != streamID {
		r.retainStoppedLocked(previous)
	}
}

func (r *observedRuntimeRegistry) retainStoppedLocked(streamID string) {
	runtime, exists := r.byStreamID[streamID]
	if !exists || runtime.State != "stopped" || r.sourceReferencesLocked(streamID) {
		return
	}
	r.removeRecentStoppedLocked(streamID)
	r.recentStopped = append(r.recentStopped, streamID)
	if len(r.recentStopped) <= maxRecentStoppedRuntimes {
		return
	}
	oldest := r.recentStopped[0]
	delete(r.byStreamID, oldest)
	copy(r.recentStopped, r.recentStopped[1:])
	r.recentStopped[len(r.recentStopped)-1] = ""
	r.recentStopped = r.recentStopped[:len(r.recentStopped)-1]
}

func (r *observedRuntimeRegistry) removeRecentStoppedLocked(streamID string) {
	for index, candidate := range r.recentStopped {
		if candidate != streamID {
			continue
		}
		copy(r.recentStopped[index:], r.recentStopped[index+1:])
		r.recentStopped[len(r.recentStopped)-1] = ""
		r.recentStopped = r.recentStopped[:len(r.recentStopped)-1]
		return
	}
}

func (r *observedRuntimeRegistry) sourceReferencesLocked(streamID string) bool {
	for _, currentStreamID := range r.currentBySource {
		if currentStreamID == streamID {
			return true
		}
	}
	return false
}

func sameRuntimeIdentity(left, right observedRuntime) bool {
	return left.ServerID == right.ServerID && left.InstanceID == right.InstanceID && left.StreamID == right.StreamID &&
		left.StreamName == right.StreamName && left.SourceID == right.SourceID && left.Kind == right.Kind &&
		left.Protocol == right.Protocol
}
