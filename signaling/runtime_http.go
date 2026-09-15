package main

import (
	"encoding/json"
	"errors"
	"net/http"
	"time"
)

type runtimeEventRequest struct {
	Kind       string          `json:"kind"`
	ServerID   string          `json:"server_id"`
	InstanceID string          `json:"instance_id"`
	StreamID   string          `json:"stream_id"`
	StreamName string          `json:"stream_name"`
	SourceID   json.RawMessage `json:"source_id"`
	Protocol   string          `json:"protocol"`
	State      string          `json:"state"`
	Stage      json.RawMessage `json:"stage"`
	EndReason  json.RawMessage `json:"end_reason"`
	Error      json.RawMessage `json:"error"`
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
	var payload runtimeEventRequest
	if !decodeJSON(writer, request, &payload) {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	event, valid := makeObservedRuntime(payload)
	if !valid {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	var applyErr error
	online := s.registry.withOnlineInstance(event.ServerID, event.InstanceID, func() {
		_, applyErr = s.runtimes.apply(event)
	})
	if !online {
		writeHTTPError(writer, http.StatusGone, "stale_instance")
		return
	}
	if errors.Is(applyErr, errRuntimeConflict) {
		writeHTTPError(writer, http.StatusConflict, "runtime_conflict")
		return
	}
	if applyErr != nil {
		s.logger.Error("apply runtime event failed", "stream_id", event.StreamID, "error", applyErr)
		writeHTTPError(writer, http.StatusInternalServerError, "operation_failed")
		return
	}
	if event.Kind == "source" && event.Protocol == "rtsp" && event.State != "stopped" && event.SourceID != "" {
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

func makeObservedRuntime(payload runtimeEventRequest) (observedRuntime, bool) {
	sourceID, sourceIDValid := decodeOptionalString(payload.SourceID)
	stage, stageValid := decodeOptionalString(payload.Stage)
	endReason, endReasonValid := decodeOptionalString(payload.EndReason)
	errorText, errorValid := decodeOptionalString(payload.Error)
	if !sourceIDValid || !stageValid || !endReasonValid || !errorValid ||
		(sourceID != nil && !validUUIDv4(*sourceID)) || (stage != nil && *stage == "") ||
		(endReason != nil && !validRuntimeEndReason(*endReason)) || (errorText != nil && *errorText == "") {
		return observedRuntime{}, false
	}
	event := observedRuntime{
		Kind: payload.Kind, ServerID: payload.ServerID, InstanceID: payload.InstanceID,
		StreamID: payload.StreamID, StreamName: payload.StreamName,
		Protocol: payload.Protocol, State: payload.State,
	}
	if sourceID != nil {
		event.SourceID = *sourceID
	}
	if stage != nil {
		event.Stage = *stage
	}
	if endReason != nil {
		event.EndReason = *endReason
	}
	if errorText != nil {
		event.Error = *errorText
	}
	return event, validRuntimeEvent(event)
}

func validRuntimeEvent(event observedRuntime) bool {
	if event.ServerID == "" || event.InstanceID == "" || !validUUIDv4(event.StreamID) || event.StreamName == "" ||
		(event.Kind != "source" && event.Kind != "publisher" && event.Kind != "output") || !validRuntimeProtocol(event.Protocol) ||
		(event.State != "starting" && event.State != "streaming" && event.State != "stopped") {
		return false
	}
	if event.State == "stopped" {
		if event.EndReason == "" {
			return false
		}
	} else if event.EndReason != "" || event.Error != "" {
		return false
	}
	return true
}

func validRuntimeProtocol(value string) bool {
	return value == "rtmp" || value == "rtsp" || value == "gb28181" || value == "whep"
}

func validRuntimeEndReason(value string) bool {
	return value == "requested" || value == "remote" || value == "timeout" || value == "protocol_error" ||
		value == "runtime_error" || value == "server_shutdown"
}
