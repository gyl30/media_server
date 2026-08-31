package main

import (
	"context"
	"log/slog"
	"net"
	"strconv"
	"sync/atomic"
	"time"

	"github.com/emiago/sipgo"
)

type sipServer struct {
	ua              *sipgo.UserAgent
	server          *sipgo.Server
	client          *sipgo.Client
	listen          string
	cfg             config
	logger          *slog.Logger
	now             func() time.Time
	auth            *digestAuthenticator
	devices         *deviceRegistry
	channels        *channelRegistry
	sweepInterval   time.Duration
	onDeviceOffline func(string)
	advertiseHost   string
	advertisePort   int
	catalogQueue    chan registeredDevice
	catalogSN       atomic.Uint32
}

func newSIPServer(cfg config, logger *slog.Logger) (*sipServer, error) {
	ua, err := sipgo.NewUA(
		sipgo.WithUserAgent("media-server-signaling"),
	)
	if err != nil {
		return nil, err
	}
	server, err := sipgo.NewServer(ua, sipgo.WithServerLogger(logger))
	if err != nil {
		_ = ua.Close()
		return nil, err
	}
	client, err := sipgo.NewClient(ua, sipgo.WithClientAddr(cfg.sipAdvertise), sipgo.WithClientConnectionAddr(cfg.sipListen))
	if err != nil {
		_ = ua.Close()
		return nil, err
	}
	advertiseHost, advertisePortText, err := net.SplitHostPort(cfg.sipAdvertise)
	if err != nil {
		_ = ua.Close()
		return nil, err
	}
	advertisePort, err := strconv.Atoi(advertisePortText)
	if err != nil {
		_ = ua.Close()
		return nil, err
	}
	result := &sipServer{
		ua:            ua,
		server:        server,
		client:        client,
		listen:        cfg.sipListen,
		cfg:           cfg,
		logger:        logger,
		now:           time.Now,
		auth:          newDigestAuthenticator(cfg.sipDomain, cfg.sipPassword),
		devices:       newDeviceRegistry(),
		channels:      newChannelRegistry(),
		sweepInterval: time.Second,
		advertiseHost: advertiseHost,
		advertisePort: advertisePort,
		catalogQueue:  make(chan registeredDevice, 256),
	}
	server.OnRegister(result.handleRegister)
	server.OnMessage(result.handleMessage)
	return result, nil
}

func (s *sipServer) serve(ctx context.Context) error {
	serveContext, cancel := context.WithCancel(ctx)
	expiryDone := make(chan struct{})
	catalogDone := make(chan struct{})
	go func() {
		defer close(expiryDone)
		s.runDeviceExpiry(serveContext.Done())
	}()
	go func() {
		defer close(catalogDone)
		s.runCatalogQueries(serveContext)
	}()
	err := s.server.ListenAndServe(serveContext, "udp", s.listen)
	cancel()
	<-expiryDone
	<-catalogDone
	if ctx.Err() != nil {
		return nil
	}
	return err
}

func (s *sipServer) close() { _ = s.ua.Close() }
