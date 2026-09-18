package main

import (
	"net"
	"net/url"
	"strconv"
)

type streamAllocationRequest struct {
	Protocol   string `json:"protocol"`
	StreamName string `json:"stream_name"`
}

func makeStreamURL(protocol, streamName, streamID string, server mediaServerInstance) string {
	port := server.rtmpPort
	if protocol == "rtsp" {
		port = server.rtspPort
	}
	streamURL := url.URL{
		Scheme: protocol,
		Host:   net.JoinHostPort(server.mediaIP, strconv.FormatUint(uint64(port), 10)),
		Path:   "/" + streamName,
	}
	query := streamURL.Query()
	query.Set("stream_id", streamID)
	streamURL.RawQuery = query.Encode()
	return streamURL.String()
}
