package main

import (
	"net/http"
	"time"
)

type playAllocationResponse struct {
	StreamID string `json:"stream_id"`
	PlayURL  string `json:"play_url"`
}

func (s *infrastructureServer) handlePlayAllocation(writer http.ResponseWriter, request *http.Request) {
	var command streamAllocationRequest
	if !decodeJSON(writer, request, &command) ||
		!validStreamAllocationProtocol(streamOperationPlay, command.Protocol) || command.StreamName == "" {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	server, ok := s.registry.selectOnline()
	if !ok {
		writeHTTPError(writer, http.StatusServiceUnavailable, "no_media_server")
		return
	}

	streamID := s.allocations.create(streamOperationPlay, command.Protocol, command.StreamName, server, time.Now())
	writeJSON(writer, http.StatusCreated, playAllocationResponse{
		StreamID: streamID, PlayURL: makeStreamURL(command.Protocol, command.StreamName, streamID, server),
	})
}
