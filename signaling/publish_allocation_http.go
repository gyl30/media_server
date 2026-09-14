package main

import (
	"net"
	"net/http"
	"net/url"
	"strconv"
	"time"
)

type publishAllocationRequest struct {
	Protocol   string `json:"protocol"`
	StreamName string `json:"stream_name"`
}

type publishAllocationResponse struct {
	StreamID   string `json:"stream_id"`
	PublishURL string `json:"publish_url"`
}

func (s *infrastructureServer) handlePublishAllocation(writer http.ResponseWriter, request *http.Request) {
	var command publishAllocationRequest
	if !decodeJSON(writer, request, &command) ||
		(command.Protocol != "rtmp" && command.Protocol != "rtsp") || command.StreamName == "" {
		writeHTTPError(writer, http.StatusBadRequest, "invalid_request")
		return
	}
	server, ok := s.registry.selectOnline()
	if !ok {
		writeHTTPError(writer, http.StatusServiceUnavailable, "no_media_server")
		return
	}

	allocation := s.allocations.create(command.Protocol, command.StreamName, server, time.Now())
	writeJSON(writer, http.StatusCreated, publishAllocationResponse{
		StreamID: allocation.streamID, PublishURL: makePublishURL(command.Protocol, command.StreamName, allocation.streamID, server),
	})
}

func makePublishURL(protocol, streamName, streamID string, server mediaServerInstance) string {
	port := server.rtmpPort
	if protocol == "rtsp" {
		port = server.rtspPort
	}
	publishURL := url.URL{
		Scheme: protocol,
		Host:   net.JoinHostPort(server.mediaIP, strconv.FormatUint(uint64(port), 10)),
		Path:   "/" + streamName,
	}
	query := publishURL.Query()
	query.Set("stream_id", streamID)
	publishURL.RawQuery = query.Encode()
	return publishURL.String()
}
