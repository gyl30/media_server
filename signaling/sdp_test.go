package main

import (
	"strings"
	"testing"
)

func TestLiveAnswerCompatibility(t *testing.T) {
	const requested uint32 = 200000001
	base := []string{
		"v=0", "o=34020000001320000002 0 0 IN IP4 192.168.1.64", "s=Play", "c=IN IP4 192.168.1.64", "t=0 0",
		"m=video 15060 RTP/AVP 96", "a=sendonly", "a=rtpmap:96 PS/90000", "y=0200000001", "f=v/2/5/25/1/4000a/1/8/1",
	}
	build := func(edit func([]string) []string) []byte {
		lines := edit(append([]string(nil), base...))
		return []byte(strings.Join(lines, "\r\n") + "\r\n")
	}
	replace := func(prefix, value string) func([]string) []string {
		return func(lines []string) []string {
			result := lines[:0]
			for _, line := range lines {
				if strings.HasPrefix(line, prefix) {
					if value == "" {
						continue
					}
					line = value
				}
				result = append(result, line)
			}
			return result
		}
	}
	cases := []struct {
		name  string
		body  []byte
		ssrc  uint32
		valid bool
	}{
		{"standard", build(func(lines []string) []string { return lines }), requested, true},
		{"missing y uses requested", build(replace("y=", "")), requested, true},
		{"device selected y", build(replace("y=", "y=0100000099")), 100000099, true},
		{"empty f", build(replace("f=", "f=")), requested, true},
		{"missing f", build(replace("f=", "")), requested, true},
		{"vendor session name", build(replace("s=", "s=Embedded Net DVR")), requested, true},
		{"omitted direction", build(replace("a=sendonly", "")), requested, true},
		{"recvonly", build(replace("a=sendonly", "a=recvonly")), 0, false},
		{"wrong payload", build(replace("m=", "m=video 15060 RTP/AVP 98")), 0, false},
		{"invalid y", build(replace("y=", "y=abc")), 0, false},
	}
	for _, item := range cases {
		ssrc, err := validateLiveUDPAnswer(item.body, 96, requested)
		if (err == nil) != item.valid || (item.valid && ssrc != item.ssrc) {
			t.Errorf("%s: ssrc=%d err=%v", item.name, ssrc, err)
		}
	}
}

func TestCatalogStatusCompatibility(t *testing.T) {
	registry := newChannelRegistry()
	registry.beginQuery("34020000001320000001", 1)
	response := catalogResponse{DeviceID: "34020000001320000001", SN: 1, SumNum: 4}
	response.DeviceList.Num = 4
	response.DeviceList.Items = []catalogChannel{
		{DeviceID: "34020000002150000001", Name: "virtual group"},
		{DeviceID: "34020000001320000002", Status: "ONLINE"},
		{DeviceID: "34020000001320000003", Status: "on"},
		{DeviceID: "34020000001320000004", Status: "OFFLINE"},
	}
	if err := registry.apply(response); err != nil {
		t.Fatalf("catalog rejected: %v", err)
	}
	expected := map[string]string{
		"34020000002150000001": "OFF", "34020000001320000002": "ON", "34020000001320000003": "ON", "34020000001320000004": "OFF",
	}
	for id, status := range expected {
		if channel, ok := registry.get("34020000001320000001", id); !ok || channel.status != status {
			t.Errorf("channel %s status %+v", id, channel)
		}
	}
}
