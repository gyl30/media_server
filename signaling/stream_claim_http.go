package main

import (
	"errors"
	"net/http"
	"time"

	"github.com/google/uuid"
)

type streamClaimRequest struct {
	StreamID   string `json:"stream_id"`
	ServerID   string `json:"server_id"`
	InstanceID string `json:"instance_id"`
	Protocol   string `json:"protocol"`
	StreamName string `json:"stream_name"`
}

func (s *infrastructureServer) handlePublishClaim(writer http.ResponseWriter, request *http.Request) {
	s.handleStreamClaim(writer, request, streamOperationPublish)
}

func (s *infrastructureServer) handlePlayClaim(writer http.ResponseWriter, request *http.Request) {
	s.handleStreamClaim(writer, request, streamOperationPlay)
}

func (s *infrastructureServer) handleStreamClaim(writer http.ResponseWriter, request *http.Request, operation streamOperation) {
	var command streamClaimRequest
	if !decodeJSON(writer, request, &command) || !validUUIDv4(command.StreamID) || command.ServerID == "" ||
		command.InstanceID == "" ||
		(command.Protocol != "rtmp" && command.Protocol != "rtsp") || command.StreamName == "" {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	var err error
	if !s.registry.withOnlineInstance(command.ServerID, command.InstanceID, func() {
		err = s.allocations.claim(command.StreamID, operation, command.Protocol, command.StreamName,
			command.ServerID, command.InstanceID, time.Now())
	}) {
		writeHTTPError(writer, http.StatusGone, "stale_instance")
		return
	}
	switch {
	case err == nil:
		writer.WriteHeader(http.StatusNoContent)
	case errors.Is(err, errStreamAllocationNotFound):
		writeHTTPError(writer, http.StatusNotFound, "allocation_not_found")
	default:
		writeHTTPError(writer, http.StatusConflict, "allocation_conflict")
	}
}

func validUUIDv4(value string) bool {
	parsed, err := uuid.Parse(value)
	return err == nil && parsed.Version() == 4 && parsed.Variant() == uuid.RFC4122 && parsed.String() == value
}
