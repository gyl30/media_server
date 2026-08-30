package main

import (
	"context"
	"encoding/xml"
	"fmt"
	"log/slog"
	"net"
	"strconv"
	"sync"
	"sync/atomic"
	"time"

	"github.com/emiago/sipgo"
	"github.com/emiago/sipgo/sip"
)

type catalogQuery struct {
	XMLName  xml.Name `xml:"Query"`
	CmdType  string   `xml:"CmdType"`
	SN       int      `xml:"SN"`
	DeviceID string   `xml:"DeviceID"`
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

type catalogChannel struct {
	DeviceID string `xml:"DeviceID"`
	Name     string `xml:"Name"`
	ParentID string `xml:"ParentID"`
	Status   string `xml:"Status"`
}

type keepaliveNotify struct {
	XMLName  xml.Name `xml:"Notify"`
	CmdType  string   `xml:"CmdType"`
	SN       int      `xml:"SN"`
	DeviceID string   `xml:"DeviceID"`
	Status   string   `xml:"Status"`
}

type simulatedDevice struct {
	cfg        config
	logger     *slog.Logger
	ua         *sipgo.UserAgent
	server     *sipgo.Server
	client     *sipgo.Client
	dialogs    atomic.Pointer[sipgo.DialogServerCache]
	listenAddr string
	serveDone  chan error
	catalog    chan error
	invite     chan mediaTarget
	ack        chan struct{}
	bye        chan struct{}
	cancel     context.CancelFunc
	closeOnce  sync.Once
}

func newSimulatedDevice(cfg config, logger *slog.Logger) (*simulatedDevice, error) {
	host, _, err := net.SplitHostPort(cfg.listen)
	if err != nil {
		return nil, err
	}
	ua, err := sipgo.NewUA(
		sipgo.WithUserAgent(cfg.deviceID),
		sipgo.WithUserAgentHostname(host),
	)
	if err != nil {
		return nil, err
	}
	server, err := sipgo.NewServer(ua, sipgo.WithServerLogger(logger))
	if err != nil {
		_ = ua.Close()
		return nil, err
	}
	device := &simulatedDevice{
		cfg:       cfg,
		logger:    logger,
		ua:        ua,
		server:    server,
		serveDone: make(chan error, 1),
		catalog:   make(chan error, 1),
		invite:    make(chan mediaTarget, 1),
		ack:       make(chan struct{}, 1),
		bye:       make(chan struct{}, 1),
	}
	server.OnMessage(device.handleMessage)
	server.OnInvite(device.handleInvite)
	server.OnAck(device.handleAck)
	server.OnBye(device.handleBye)
	return device, nil
}

func (d *simulatedDevice) start(ctx context.Context) error {
	serveContext, cancel := context.WithCancel(ctx)
	d.cancel = cancel
	ready := make(chan string, 1)
	serveContext = context.WithValue(serveContext, sipgo.ListenReadyCtxKey, sipgo.ListenReadyFuncCtxValue(func(_ string, addr string) {
		ready <- addr
	}))
	go func() { d.serveDone <- d.server.ListenAndServe(serveContext, "udp", d.cfg.listen) }()
	select {
	case addr := <-ready:
		d.listenAddr = addr
	case err := <-d.serveDone:
		cancel()
		return fmt.Errorf("SIP server stopped before ready: %w", err)
	case <-ctx.Done():
		cancel()
		return ctx.Err()
	}
	client, err := sipgo.NewClient(d.ua,
		sipgo.WithClientConnectionAddr(d.listenAddr),
		sipgo.WithClientAddr(d.listenAddr),
		sipgo.WithClientNAT(),
	)
	if err != nil {
		cancel()
		return err
	}
	d.client = client
	host, portText, err := net.SplitHostPort(d.listenAddr)
	if err != nil {
		cancel()
		return err
	}
	port, err := strconv.Atoi(portText)
	if err != nil {
		cancel()
		return err
	}
	d.dialogs.Store(sipgo.NewDialogServerCache(client, sip.ContactHeader{Address: sip.Uri{
		Scheme: "sip", User: d.cfg.deviceID, Host: host, Port: port,
	}}))
	d.logger.Info("simulator SIP UDP listening", "address", d.listenAddr, "device_id", d.cfg.deviceID)
	return nil
}

func (d *simulatedDevice) close() {
	d.closeOnce.Do(func() {
		if d.cancel != nil {
			d.cancel()
		}
		select {
		case <-d.serveDone:
		case <-time.After(2 * time.Second):
		}
		_ = d.ua.Close()
	})
}

func (d *simulatedDevice) register(ctx context.Context) error {
	host, portText, err := net.SplitHostPort(d.listenAddr)
	if err != nil {
		return err
	}
	port, err := strconv.Atoi(portText)
	if err != nil {
		return err
	}
	recipient, err := d.platformURI(false)
	if err != nil {
		return err
	}
	request := sip.NewRequest(sip.REGISTER, recipient)
	fromParams := sip.NewParams()
	fromParams.Add("tag", sip.GenerateTagN(16))
	request.AppendHeader(&sip.FromHeader{
		Address: sip.Uri{Scheme: "sip", User: d.cfg.deviceID, Host: d.cfg.domain}, Params: fromParams,
	})
	request.AppendHeader(&sip.ToHeader{Address: sip.Uri{Scheme: "sip", User: d.cfg.deviceID, Host: d.cfg.domain}})
	request.AppendHeader(&sip.ContactHeader{Address: sip.Uri{Scheme: "sip", User: d.cfg.deviceID, Host: host, Port: port}})
	expires := sip.ExpiresHeader(uint32(d.cfg.registerExpiry / time.Second))
	request.AppendHeader(&expires)
	request.SetTransport("UDP")
	request.SetDestination(d.cfg.platformSIP)
	challenge, err := d.client.Do(ctx, request)
	if err != nil {
		return err
	}
	if challenge.StatusCode != sip.StatusUnauthorized {
		return fmt.Errorf("REGISTER challenge status %d", challenge.StatusCode)
	}
	response, err := d.client.DoDigestAuth(ctx, request, challenge, sipgo.DigestAuth{
		Username: d.cfg.deviceID,
		Password: d.cfg.password,
	})
	if err != nil {
		return err
	}
	if response.StatusCode != sip.StatusOK {
		return fmt.Errorf("REGISTER status %d", response.StatusCode)
	}
	d.logger.Info("simulator registered", "device_id", d.cfg.deviceID)
	return nil
}

func (d *simulatedDevice) keepalive(ctx context.Context, sn int) error {
	body, err := xml.Marshal(keepaliveNotify{CmdType: "Keepalive", SN: sn, DeviceID: d.cfg.deviceID, Status: "OK"})
	if err != nil {
		return err
	}
	return d.sendMANSCDP(ctx, append([]byte(xml.Header), body...))
}

func (d *simulatedDevice) runHeartbeats(ctx context.Context, errors chan<- error) {
	ticker := time.NewTicker(d.cfg.heartbeat)
	defer ticker.Stop()
	sn := 2
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			requestContext, cancel := context.WithTimeout(ctx, 3*time.Second)
			err := d.keepalive(requestContext, sn)
			cancel()
			if err != nil {
				select {
				case errors <- err:
				default:
				}
				return
			}
			sn++
		}
	}
}

func (d *simulatedDevice) handleMessage(request *sip.Request, transaction sip.ServerTransaction) {
	if err := transaction.Respond(sip.NewResponseFromRequest(request, sip.StatusOK, "OK", nil)); err != nil {
		d.publishCatalog(fmt.Errorf("respond Catalog query: %w", err))
		return
	}
	var query catalogQuery
	if err := xml.Unmarshal(request.Body(), &query); err != nil || query.CmdType != "Catalog" || query.DeviceID != d.cfg.deviceID || query.SN <= 0 {
		return
	}
	go func() {
		ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		defer cancel()
		d.publishCatalog(d.sendCatalog(ctx, query.SN))
	}()
}

func (d *simulatedDevice) sendCatalog(ctx context.Context, sn int) error {
	response := catalogResponse{CmdType: "Catalog", SN: sn, DeviceID: d.cfg.deviceID, SumNum: 1}
	response.DeviceList.Num = 1
	response.DeviceList.Items = []catalogChannel{{
		DeviceID: d.cfg.channelID, Name: "simulator camera", ParentID: d.cfg.deviceID, Status: "ON",
	}}
	body, err := xml.Marshal(response)
	if err != nil {
		return err
	}
	if err := d.sendMANSCDP(ctx, append([]byte(xml.Header), body...)); err != nil {
		return err
	}
	d.logger.Info("simulator Catalog sent", "device_id", d.cfg.deviceID, "channel_id", d.cfg.channelID)
	return nil
}

func (d *simulatedDevice) publishCatalog(err error) {
	select {
	case d.catalog <- err:
	default:
	}
}

func (d *simulatedDevice) sendMANSCDP(ctx context.Context, body []byte) error {
	recipient, err := d.platformURI(true)
	if err != nil {
		return err
	}
	request := sip.NewRequest(sip.MESSAGE, recipient)
	fromParams := sip.NewParams()
	fromParams.Add("tag", sip.GenerateTagN(16))
	request.AppendHeader(&sip.FromHeader{
		Address: sip.Uri{Scheme: "sip", User: d.cfg.deviceID, Host: d.cfg.domain}, Params: fromParams,
	})
	request.AppendHeader(&sip.ToHeader{Address: sip.Uri{Scheme: "sip", User: d.cfg.platformID, Host: d.cfg.domain}})
	request.AppendHeader(sip.NewHeader("Content-Type", "Application/MANSCDP+xml"))
	request.SetBody(body)
	request.SetTransport("UDP")
	request.SetDestination(d.cfg.platformSIP)
	response, err := d.client.Do(ctx, request)
	if err != nil {
		return err
	}
	if !response.IsSuccess() {
		return fmt.Errorf("MESSAGE rejected with SIP %d", response.StatusCode)
	}
	return nil
}

func (d *simulatedDevice) handleInvite(request *sip.Request, transaction sip.ServerTransaction) {
	target, answer, err := parseLiveOffer(request.Body())
	if err != nil || request.Recipient.User != d.cfg.channelID {
		_ = transaction.Respond(sip.NewResponseFromRequest(request, sip.StatusBadRequest, "Bad Request", nil))
		return
	}
	cache := d.dialogs.Load()
	if cache == nil {
		_ = transaction.Respond(sip.NewResponseFromRequest(request, sip.StatusServiceUnavailable, "Service Unavailable", nil))
		return
	}
	dialog, err := cache.ReadInvite(request, transaction)
	if err != nil {
		d.logger.Error("simulator ReadInvite failed", "error", err)
		return
	}
	if !transaction.OnCancel(func(*sip.Request) { _ = dialog.Close() }) {
		_ = dialog.Close()
		return
	}
	select {
	case d.invite <- target:
	default:
	}
	if err := dialog.Respond(sip.StatusTrying, "Trying", nil); err != nil {
		d.logger.Error("simulator INVITE provisional failed", "error", err)
		return
	}
	if err := dialog.RespondSDP(answer); err != nil && err.Error() != "No ACK received" {
		d.logger.Error("simulator INVITE answer failed", "error", err)
	}
}

func (d *simulatedDevice) handleAck(request *sip.Request, transaction sip.ServerTransaction) {
	cache := d.dialogs.Load()
	if cache == nil {
		return
	}
	if err := cache.ReadAck(request, transaction); err != nil {
		d.logger.Warn("simulator ACK failed", "error", err)
		return
	}
	select {
	case d.ack <- struct{}{}:
	default:
	}
}

func (d *simulatedDevice) handleBye(request *sip.Request, transaction sip.ServerTransaction) {
	cache := d.dialogs.Load()
	if cache == nil {
		_ = transaction.Respond(sip.NewResponseFromRequest(request, sip.StatusCallTransactionDoesNotExists, "Call/Transaction Does Not Exist", nil))
		return
	}
	if err := cache.ReadBye(request, transaction); err != nil {
		d.logger.Warn("simulator BYE failed", "error", err)
		return
	}
	select {
	case d.bye <- struct{}{}:
	default:
	}
}

func (d *simulatedDevice) platformURI(withUser bool) (sip.Uri, error) {
	host, portText, err := net.SplitHostPort(d.cfg.platformSIP)
	if err != nil {
		return sip.Uri{}, err
	}
	port, err := strconv.Atoi(portText)
	if err != nil {
		return sip.Uri{}, err
	}
	uri := sip.Uri{Scheme: "sip", Host: host, Port: port}
	if withUser {
		uri.User = d.cfg.platformID
	}
	return uri, nil
}
