package main

import (
	"encoding/xml"
	"fmt"
	"io"
	"mime"
	"strings"
	"time"

	"github.com/emiago/sipgo/sip"
	"golang.org/x/text/encoding/simplifiedchinese"
)

type manscdpEnvelope struct {
	XMLName  xml.Name
	CmdType  string `xml:"CmdType"`
	SN       int    `xml:"SN"`
	DeviceID string `xml:"DeviceID"`
	Status   string `xml:"Status"`
}

func (s *sipServer) handleMessage(req *sip.Request, tx sip.ServerTransaction) {
	if req.Recipient.User != s.cfg.sipID {
		s.respond(tx, sip.NewResponseFromRequest(req, sip.StatusBadRequest, "Bad Request", nil))
		return
	}
	contentType := req.ContentType()
	if contentType == nil {
		s.respond(tx, sip.NewResponseFromRequest(req, sip.StatusUnsupportedMediaType, "Unsupported Media Type", nil))
		return
	}
	mediaType, _, err := mime.ParseMediaType(contentType.Value())
	if err != nil || !strings.EqualFold(mediaType, "Application/MANSCDP+xml") || len(req.Body()) > 64*1024 {
		s.respond(tx, sip.NewResponseFromRequest(req, sip.StatusUnsupportedMediaType, "Unsupported Media Type", nil))
		return
	}
	var message manscdpEnvelope
	if err := decodeMANSCDP(req.Body(), &message); err != nil || message.SN <= 0 || !validDigits(message.DeviceID, 20) {
		s.respond(tx, sip.NewResponseFromRequest(req, sip.StatusBadRequest, "Bad Request", nil))
		return
	}
	if message.CmdType == "Catalog" && message.XMLName.Local == "Response" {
		s.handleCatalog(req, tx)
		return
	}
	if message.CmdType != "Keepalive" || message.XMLName.Local != "Notify" || message.Status != "OK" {
		s.respond(tx, sip.NewResponseFromRequest(req, sip.StatusBadRequest, "Bad Request", nil))
		return
	}
	from := req.From()
	if from == nil || from.Address.User != message.DeviceID {
		s.respond(tx, sip.NewResponseFromRequest(req, sip.StatusBadRequest, "Bad Request", nil))
		return
	}
	if !s.devices.keepalive(message.DeviceID, req.Source(), s.now()) {
		s.respond(tx, sip.NewResponseFromRequest(req, sip.StatusForbidden, "Forbidden", nil))
		return
	}
	s.respond(tx, sip.NewResponseFromRequest(req, sip.StatusOK, "OK", nil))
}

func decodeMANSCDP(body []byte, target any) error {
	decoder := xml.NewDecoder(strings.NewReader(string(body)))
	decoder.CharsetReader = func(charset string, input io.Reader) (io.Reader, error) {
		switch strings.ToLower(charset) {
		case "gb2312", "gbk":
			return simplifiedchinese.GBK.NewDecoder().Reader(input), nil
		default:
			return nil, fmt.Errorf("unsupported XML charset %q", charset)
		}
	}
	if err := decoder.Decode(target); err != nil {
		return err
	}
	var extra any
	if err := decoder.Decode(&extra); err != io.EOF {
		return fmt.Errorf("unexpected XML content")
	}
	return nil
}

func (s *sipServer) expireDevices(now time.Time) {
	s.channels.expire(now)
	for _, deviceID := range s.devices.expire(now, s.cfg.heartbeatTimeout) {
		s.notifyDeviceOffline(deviceID)
	}
}

func (s *sipServer) notifyDeviceOffline(deviceID string) {
	if s.onDeviceOffline != nil {
		s.onDeviceOffline(deviceID)
	}
}

func (s *sipServer) runDeviceExpiry(ctxDone <-chan struct{}) {
	ticker := time.NewTicker(s.sweepInterval)
	defer ticker.Stop()
	for {
		select {
		case <-ctxDone:
			return
		case now := <-ticker.C:
			s.expireDevices(now)
		}
	}
}
