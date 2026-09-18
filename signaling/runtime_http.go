package main

import (
	"encoding/json"
	"errors"
	"net/http"
	"time"
)

type runtimeEventRequest struct {
	Kind       string          `json:"kind"`
	StreamID   string          `json:"stream_id"`
	StreamName string          `json:"stream_name"`
	SourceID   json.RawMessage `json:"source_id"`
	Protocol   string          `json:"protocol"`
	State      string          `json:"state"`
	Stage      json.RawMessage `json:"stage"`
	Error      json.RawMessage `json:"error"`
}

type runtimeEventBatchRequest struct {
	ServerID   string                `json:"server_id"`
	InstanceID string                `json:"instance_id"`
	Events     []runtimeEventRequest `json:"events"`
}

type mediaServerResponse struct {
	ServerID   string    `json:"server_id"`
	InstanceID string    `json:"instance_id"`
	Online     bool      `json:"online"`
	MediaIP    string    `json:"media_ip"`
	RTMPPort   uint16    `json:"rtmp_port"`
	RTSPPort   uint16    `json:"rtsp_port"`
	HTTPPort   uint16    `json:"http_port"`
	LastSeen   time.Time `json:"last_seen"`
}

func (s *infrastructureServer) handleRuntimeEvent(writer http.ResponseWriter, request *http.Request) {
	var payload runtimeEventBatchRequest
	if !decodeJSON(writer, request, &payload) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	if payload.ServerID == "" || payload.InstanceID == "" || len(payload.Events) == 0 {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	events := make([]observedRuntime, len(payload.Events))
	latest := make(map[string]observedRuntime)
	for index, eventPayload := range payload.Events {
		event, valid := makeObservedRuntime(eventPayload, payload.ServerID, payload.InstanceID)
		if !valid {
			writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
			return
		}
		if previous, exists := latest[event.StreamID]; exists {
			if !sameRuntimeIdentity(previous, event) {
				writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
				return
			}
		}
		events[index] = event
		latest[event.StreamID] = event
	}

	var applyErr error
	var applied []observedRuntime
	online := s.registry.withOnlineInstance(payload.ServerID, payload.InstanceID, func() {
		applied, applyErr = s.runtimes.applyBatch(events)
	})
	if !online {
		writeHTTPError(writer, http.StatusGone, "stale_instance")
		return
	}
	for _, event := range applied {
		if event.Kind == "source" && event.Protocol == "rtsp" &&
			(event.State == "starting" || event.State == "streaming") && event.SourceID != "" {
			s.confirmRTSPPull(rtspPullRuntime{
				sourceID: event.SourceID, streamName: event.StreamName,
				server: mediaServerInstance{serverID: event.ServerID, instanceID: event.InstanceID}, streamID: event.StreamID,
			})
		}
		if event.State == "stopped" {
			if event.Kind == "source" && event.Protocol == "rtsp" && event.SourceID != "" {
				s.removeRTSPPull(rtspPullRuntime{
					sourceID: event.SourceID, streamName: event.StreamName,
					server: mediaServerInstance{serverID: event.ServerID, instanceID: event.InstanceID}, streamID: event.StreamID,
				})
			}
			if s.live != nil && event.Kind == "source" && event.Protocol == "gb28181" {
				s.live.runtimeStopped(event.ServerID, event.InstanceID, event.StreamID, event.StreamName)
			}
		}
	}
	if errors.Is(applyErr, errRuntimeConflict) {
		writeHTTPError(writer, http.StatusConflict, "runtime_conflict")
		return
	}
	if applyErr != nil {
		s.logger.Error("apply runtime event failed", "error", applyErr)
		writeHTTPError(writer, http.StatusInternalServerError, "operation_failed")
		return
	}
	writer.WriteHeader(http.StatusNoContent)
}

func (s *infrastructureServer) handleRuntimeList(writer http.ResponseWriter, _ *http.Request) {
	writeJSON(writer, http.StatusOK, map[string]any{"runtimes": s.runtimes.snapshot()})
}

func (s *infrastructureServer) handleMediaServerList(writer http.ResponseWriter, _ *http.Request) {
	instances := s.registry.currentInstances()
	response := make([]mediaServerResponse, 0, len(instances))
	for _, instance := range instances {
		response = append(response, mediaServerResponse{
			ServerID: instance.serverID, InstanceID: instance.instanceID, Online: instance.online,
			MediaIP: instance.mediaIP, RTMPPort: instance.rtmpPort, RTSPPort: instance.rtspPort,
			HTTPPort: instance.httpPort, LastSeen: instance.lastHeartbeat,
		})
	}
	writeJSON(writer, http.StatusOK, map[string]any{"media_servers": response})
}

func makeObservedRuntime(payload runtimeEventRequest, serverID, instanceID string) (observedRuntime, bool) {
	sourceID, sourceIDValid := decodeOptionalString(payload.SourceID)
	stage, stageValid := decodeOptionalString(payload.Stage)
	errorText, errorValid := decodeOptionalString(payload.Error)
	if !sourceIDValid || !stageValid || !errorValid ||
		(sourceID != nil && !validUUIDv4(*sourceID)) || (stage != nil && *stage == "") ||
		(errorText != nil && *errorText == "") {
		return observedRuntime{}, false
	}
	event := observedRuntime{
		Kind: payload.Kind, ServerID: serverID, InstanceID: instanceID,
		StreamID: payload.StreamID, StreamName: payload.StreamName,
		Protocol: payload.Protocol, State: payload.State,
	}
	if sourceID != nil {
		event.SourceID = *sourceID
	}
	if stage != nil {
		event.Stage = *stage
	}
	if errorText != nil {
		event.Error = *errorText
	}
	return event, validRuntimeEvent(event)
}

func validRuntimeEvent(event observedRuntime) bool {
	if event.ServerID == "" || event.InstanceID == "" || !validUUIDv4(event.StreamID) || event.StreamName == "" ||
		(event.Kind != "source" && event.Kind != "publisher" && event.Kind != "output") || !validRuntimeProtocol(event.Protocol) ||
		(event.State != "starting" && event.State != "streaming" && event.State != "stopped" && event.State != "stop_requested" &&
			event.State != "remote_closed" && event.State != "timeout" && event.State != "protocol_error" && event.State != "runtime_error") ||
		(event.Kind == "source" && event.Protocol == "rtsp" && event.SourceID == "") {
		return false
	}
	if isLifecycleState(event.State) && event.Error != "" {
		return false
	}
	return true
}

func validRuntimeProtocol(value string) bool {
	return value == "rtmp" || value == "rtsp" || value == "gb28181" || value == "whep" || value == "whip" ||
		value == "http-flv" || value == "hls"
}
