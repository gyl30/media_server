package main

import (
	"net/http"
	"time"
)

type publishAllocationResponse struct {
	StreamID   string `json:"stream_id"`
	PublishURL string `json:"publish_url"`
}

func (s *infrastructureServer) handlePublishAllocation(writer http.ResponseWriter, request *http.Request) {
	var command streamAllocationRequest
	if !decodeJSON(writer, request, &command) ||
		!validStreamAllocationProtocol(streamOperationPublish, command.Protocol) || command.StreamName == "" {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	server, ok := s.registry.selectOnline()
	if !ok {
		writeHTTPError(writer, http.StatusServiceUnavailable, "no_media_server")
		return
	}

	streamID := s.allocations.create(streamOperationPublish, command.Protocol, command.StreamName, server, time.Now())
	writeJSON(writer, http.StatusCreated, publishAllocationResponse{
		StreamID: streamID, PublishURL: makeStreamURL(command.Protocol, command.StreamName, streamID, server),
	})
}
