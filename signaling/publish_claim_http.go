package main

import (
	"errors"
	"net/http"
	"time"

	"github.com/google/uuid"
)

type publishClaimRequest struct {
	StreamID   string `json:"stream_id"`
	ServerID   string `json:"server_id"`
	InstanceID string `json:"instance_id"`
	Direction  string `json:"direction"`
	Protocol   string `json:"protocol"`
	StreamName string `json:"stream_name"`
}

func (s *infrastructureServer) handlePublishClaim(writer http.ResponseWriter, request *http.Request) {
	var command publishClaimRequest
	if !decodeJSON(writer, request, &command) || !validUUIDv4(command.StreamID) || command.ServerID == "" ||
		command.InstanceID == "" || command.Direction != "input" ||
		(command.Protocol != "rtmp" && command.Protocol != "rtsp") || command.StreamName == "" {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	var err error
	if !s.registry.withOnlineInstance(command.ServerID, command.InstanceID, func() {
		err = s.allocations.claim(publishClaim{
			streamID: command.StreamID, serverID: command.ServerID, instanceID: command.InstanceID,
			direction: command.Direction, protocol: command.Protocol, streamName: command.StreamName,
		}, time.Now())
	}) {
		writeHTTPError(writer, http.StatusGone, "stale_instance")
		return
	}
	switch {
	case err == nil:
		writeJSON(writer, http.StatusOK, map[string]string{"result": "ok"})
	case errors.Is(err, errPublishAllocationNotFound):
		writeHTTPError(writer, http.StatusNotFound, "allocation_not_found")
	case errors.Is(err, errPublishAllocationExpired):
		writeHTTPError(writer, http.StatusGone, "allocation_expired")
	default:
		writeHTTPError(writer, http.StatusConflict, "allocation_conflict")
	}
}

func validUUIDv4(value string) bool {
	parsed, err := uuid.Parse(value)
	return err == nil && parsed.Version() == 4 && parsed.Variant() == uuid.RFC4122 && parsed.String() == value
}
