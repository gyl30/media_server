package main

import (
	"context"
	"encoding/xml"
	"fmt"
	"strconv"
	"time"

	"github.com/emiago/sipgo/sip"
)

type catalogQuery struct {
	XMLName  xml.Name `xml:"Query"`
	CmdType  string   `xml:"CmdType"`
	SN       int      `xml:"SN"`
	DeviceID string   `xml:"DeviceID"`
}

type catalogChannel struct {
	DeviceID string `xml:"DeviceID"`
	Name     string `xml:"Name"`
	ParentID string `xml:"ParentID"`
	Status   string `xml:"Status"`
}

type catalogResponse struct {
	XMLName    xml.Name `xml:"Response"`
	CmdType    string   `xml:"CmdType"`
	SN         int      `xml:"SN"`
	DeviceID   string   `xml:"DeviceID"`
	SumNum     int      `xml:"SumNum"`
	DeviceList struct {
		Num   int              `xml:"Num,attr"`
		Items []catalogChannel `xml:"Item"`
	} `xml:"DeviceList"`
}

func buildCatalogQuery(deviceID string, sn int) ([]byte, error) {
	body, err := xml.Marshal(catalogQuery{CmdType: "Catalog", SN: sn, DeviceID: deviceID})
	if err != nil {
		return nil, err
	}
	return append([]byte(xml.Header), body...), nil
}

func (s *sipServer) handleCatalog(req *sip.Request, tx sip.ServerTransaction) {
	var response catalogResponse
	if err := decodeMANSCDP(req.Body(), &response); err != nil || response.CmdType != "Catalog" || response.SN <= 0 || !validDigits(response.DeviceID, 20) {
		s.respond(tx, sip.NewResponseFromRequest(req, sip.StatusBadRequest, "Bad Request", nil))
		return
	}
	from := req.From()
	if from == nil || from.Address.User != response.DeviceID {
		s.respond(tx, sip.NewResponseFromRequest(req, sip.StatusBadRequest, "Bad Request", nil))
		return
	}
	if !s.devices.registeredSource(response.DeviceID, req.Source(), s.now()) {
		s.respond(tx, sip.NewResponseFromRequest(req, sip.StatusForbidden, "Forbidden", nil))
		return
	}
	if err := s.channels.apply(response); err != nil {
		s.respond(tx, sip.NewResponseFromRequest(req, sip.StatusBadRequest, "Bad Request", nil))
		return
	}
	s.respond(tx, sip.NewResponseFromRequest(req, sip.StatusOK, "OK", nil))
}

func (s *sipServer) runCatalogQueries(ctx context.Context) {
	for {
		select {
		case <-ctx.Done():
			return
		case device := <-s.catalogQueue:
			current, ok := s.devices.getOnline(device.id, s.now())
			if !ok {
				continue
			}
			device = current
			sn := int(s.catalogSN.Add(1))
			s.channels.beginQuery(device.id, sn)
			queryContext, cancel := context.WithTimeout(ctx, 5*time.Second)
			err := s.sendCatalogQuery(queryContext, device, sn)
			cancel()
			if err != nil {
				s.channels.cancelQuery(device.id, sn)
				s.logger.Warn("Catalog query failed", "device_id", device.id, "error", err)
			}
		}
	}
}

func (s *sipServer) sendCatalogQuery(ctx context.Context, device registeredDevice, sn int) error {
	body, err := buildCatalogQuery(device.id, sn)
	if err != nil {
		return err
	}
	recipient := *device.contact.Clone()
	request := sip.NewRequest(sip.MESSAGE, recipient)
	fromParams := sip.NewParams()
	fromParams.Add("tag", strconv.FormatUint(uint64(sn), 10))
	request.AppendHeader(&sip.FromHeader{
		Address: sip.Uri{Scheme: "sip", User: s.cfg.sipID, Host: s.cfg.sipDomain},
		Params:  fromParams,
	})
	request.AppendHeader(&sip.ToHeader{Address: recipient})
	request.AppendHeader(&sip.ContactHeader{Address: sip.Uri{
		Scheme: "sip",
		User:   s.cfg.sipID,
		Host:   s.advertiseHost,
		Port:   s.advertisePort,
	}})
	request.AppendHeader(sip.NewHeader("Content-Type", "Application/MANSCDP+xml"))
	request.SetBody(body)
	request.SetTransport("UDP")
	request.SetDestination(device.remoteEndpoint)
	response, err := s.client.Do(ctx, request)
	if err != nil {
		return err
	}
	if !response.IsSuccess() {
		return fmt.Errorf("Catalog rejected with SIP %d", response.StatusCode)
	}
	return nil
}

func (s *sipServer) enqueueCatalog(device registeredDevice) {
	select {
	case s.catalogQueue <- device:
	default:
		s.logger.Warn("Catalog queue full", "device_id", device.id)
	}
}
