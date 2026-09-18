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
	scheme := protocol
	path := "/" + streamName
	port := server.rtmpPort
	switch protocol {
	case "rtsp":
		port = server.rtspPort
	case "http-flv":
		scheme = "http"
		path += ".flv"
		port = server.httpPort
	case "hls":
		scheme = "http"
		path = "/play/hls/" + streamName + "/index.m3u8"
		port = server.httpPort
	}
	streamURL := url.URL{
		Scheme: scheme,
		Host:   net.JoinHostPort(server.mediaIP, strconv.FormatUint(uint64(port), 10)),
		Path:   path,
	}
	query := streamURL.Query()
	query.Set("stream_id", streamID)
	streamURL.RawQuery = query.Encode()
	return streamURL.String()
}

func validStreamAllocationProtocol(operation streamOperation, protocol string) bool {
	return protocol == "rtmp" || protocol == "rtsp" ||
		(operation == streamOperationPlay && (protocol == "http-flv" || protocol == "hls"))
}
