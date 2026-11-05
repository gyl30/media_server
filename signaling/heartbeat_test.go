package main

import (
	"fmt"
	"io"
	"log/slog"
	"testing"
	"time"

	"github.com/emiago/sipgo/sip"
)

type recordingTransaction struct {
	sip.ServerTransaction
	responses []*sip.Response
}

func (t *recordingTransaction) Respond(response *sip.Response) error {
	t.responses = append(t.responses, response)
	return nil
}

func TestKeepaliveStatus(t *testing.T) {
	const platformID = "34020000002000000001"
	const deviceID = "34020000001320000001"
	const source = "198.51.100.7:5061"
	now := time.Date(2026, 11, 5, 12, 0, 0, 0, time.UTC)
	cases := []struct {
		name    string
		cmdType string
		sn      string
		status  int
	}{
		{"first keepalive after slot enable carries SN 0", "Keepalive", "0", sip.StatusOK},
		{"regular keepalive", "Keepalive", "17", sip.StatusOK},
		{"negative SN", "Keepalive", "-1", sip.StatusBadRequest},
		{"other notify keeps SN above 0", "Alarm", "0", sip.StatusBadRequest},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			server := &sipServer{cfg: config{sipID: platformID}, logger: slog.New(slog.NewTextHandler(io.Discard, nil)),
				now: func() time.Time { return now }, devices: newDeviceRegistry(), live: &liveService{stoppingDevices: map[string]struct{}{}}}
			server.devices.register(registeredDevice{id: deviceID, remoteEndpoint: source, expiresAt: now.Add(time.Hour), lastHeartbeat: now, online: true})
			request := sip.NewRequest(sip.MESSAGE, sip.Uri{User: platformID, Host: "198.51.100.1", Port: 15060})
			request.AppendHeader(&sip.FromHeader{Address: sip.Uri{User: deviceID, Host: "198.51.100.7", Port: 5061}, Params: sip.NewParams()})
			request.AppendHeader(sip.NewHeader("Content-Type", "Application/MANSCDP+xml"))
			request.SetBody([]byte(fmt.Sprintf("<?xml version=\"1.0\"?>\r\n<Notify><CmdType>%s</CmdType><SN>%s</SN><DeviceID>%s</DeviceID><Status>OK</Status></Notify>\r\n", c.cmdType, c.sn, deviceID)))
			request.SetSource(source)
			transaction := &recordingTransaction{}
			server.handleMessage(request, transaction)
			if len(transaction.responses) != 1 || transaction.responses[0].StatusCode != c.status {
				t.Fatalf("responses %v, want one %d", transaction.responses, c.status)
			}
		})
	}
}
