package main

import (
	"flag"
	"fmt"
	"io"
	"net"
	"net/netip"
	"net/url"
	"strings"
	"time"
)

type config struct {
	database            string
	sipListen           string
	sipAdvertise        string
	httpListen          string
	sipID               string
	sipDomain           string
	sipPassword         string
	registerExpires     time.Duration
	heartbeatTimeout    time.Duration
	mediaServer         mediaServer
	mediaRequestTimeout time.Duration
	inviteTimeout       time.Duration
	byeTimeout          time.Duration
	tokenTTL            time.Duration
}

func parseConfig(args []string) (config, error) {
	var cfg config
	var mediaHTTPPort, mediaRTMPPort, mediaRTSPPort uint
	flags := flag.NewFlagSet("signaling", flag.ContinueOnError)
	flags.SetOutput(io.Discard)
	flags.StringVar(&cfg.database, "database", "signaling.db", "SQLite database path")
	flags.StringVar(&cfg.sipListen, "sip-listen", "127.0.0.1:5060", "SIP UDP listen address")
	flags.StringVar(&cfg.sipAdvertise, "sip-advertise", "127.0.0.1:5060", "SIP address advertised to devices")
	flags.StringVar(&cfg.httpListen, "http-listen", "127.0.0.1:9090", "internal HTTP listen address")
	flags.StringVar(&cfg.sipID, "sip-id", "34020000002000000001", "GB28181 platform ID")
	flags.StringVar(&cfg.sipDomain, "sip-domain", "3402000000", "GB28181 SIP domain and Digest realm")
	flags.StringVar(&cfg.sipPassword, "sip-password", "12345678", "shared GB28181 device password")
	flags.DurationVar(&cfg.registerExpires, "register-expires", time.Hour, "default registration lifetime")
	flags.DurationVar(&cfg.heartbeatTimeout, "heartbeat-timeout", 90*time.Second, "device heartbeat timeout")
	flags.StringVar(&cfg.mediaServer.controlURL, "media-control-url", "http://127.0.0.1:8080", "media server control HTTP base URL")
	flags.StringVar(&cfg.mediaServer.controlToken, "media-control-token", "", "bearer token for the media server control API")
	flags.StringVar(&cfg.mediaServer.mediaIP, "media-ip", "127.0.0.1", "media server address advertised to clients and devices")
	flags.UintVar(&mediaHTTPPort, "media-http-port", 8080, "media server HTTP port")
	flags.UintVar(&mediaRTMPPort, "media-rtmp-port", 1935, "media server RTMP port")
	flags.UintVar(&mediaRTSPPort, "media-rtsp-port", 8554, "media server RTSP port")
	flags.DurationVar(&cfg.tokenTTL, "token-ttl", 60*time.Second, "lifetime of unconsumed stream authorization tokens")
	flags.DurationVar(&cfg.mediaRequestTimeout, "media-request-timeout", 3*time.Second, "media server HTTP request timeout")
	flags.DurationVar(&cfg.inviteTimeout, "invite-timeout", 10*time.Second, "live INVITE timeout")
	flags.DurationVar(&cfg.byeTimeout, "bye-timeout", 3*time.Second, "live BYE timeout")
	if err := flags.Parse(args); err != nil {
		return config{}, err
	}
	if flags.NArg() != 0 {
		return config{}, fmt.Errorf("unexpected argument %q", flags.Arg(0))
	}
	if cfg.database == "" {
		return config{}, fmt.Errorf("invalid database path")
	}
	sipListen, err := netip.ParseAddrPort(cfg.sipListen)
	if err != nil || sipListen.Addr().IsUnspecified() || sipListen.Port() == 0 {
		return config{}, fmt.Errorf("invalid SIP listen address")
	}
	httpListen, err := netip.ParseAddrPort(cfg.httpListen)
	if err != nil || httpListen.Addr().IsUnspecified() {
		return config{}, fmt.Errorf("invalid HTTP listen address")
	}
	advertise, err := net.ResolveUDPAddr("udp", cfg.sipAdvertise)
	if err != nil || advertise.IP == nil || advertise.IP.IsUnspecified() || advertise.Port == 0 {
		return config{}, fmt.Errorf("invalid SIP advertise address")
	}
	if !validDigits(cfg.sipID, 20) {
		return config{}, fmt.Errorf("invalid SIP platform ID")
	}
	if !validDigits(cfg.sipDomain, 10) {
		return config{}, fmt.Errorf("invalid SIP domain")
	}
	if cfg.sipPassword == "" {
		return config{}, fmt.Errorf("SIP password is empty")
	}
	if cfg.registerExpires <= 0 || cfg.registerExpires%time.Second != 0 {
		return config{}, fmt.Errorf("invalid registration lifetime")
	}
	if cfg.heartbeatTimeout <= 0 {
		return config{}, fmt.Errorf("invalid heartbeat timeout")
	}
	controlURL, err := url.Parse(cfg.mediaServer.controlURL)
	if err != nil || controlURL.Scheme != "http" || controlURL.Host == "" || controlURL.User != nil ||
		(controlURL.Path != "" && controlURL.Path != "/") || controlURL.RawQuery != "" || controlURL.Fragment != "" {
		return config{}, fmt.Errorf("invalid media control URL")
	}
	cfg.mediaServer.controlURL = strings.TrimSuffix(cfg.mediaServer.controlURL, "/")
	mediaIP := net.ParseIP(cfg.mediaServer.mediaIP)
	if mediaIP == nil || mediaIP.IsUnspecified() {
		return config{}, fmt.Errorf("invalid media IP")
	}
	if mediaHTTPPort == 0 || mediaHTTPPort > 65535 {
		return config{}, fmt.Errorf("invalid media HTTP port")
	}
	cfg.mediaServer.httpPort = uint16(mediaHTTPPort)
	if mediaRTMPPort == 0 || mediaRTMPPort > 65535 || mediaRTSPPort == 0 || mediaRTSPPort > 65535 {
		return config{}, fmt.Errorf("invalid media RTMP or RTSP port")
	}
	cfg.mediaServer.rtmpPort = uint16(mediaRTMPPort)
	cfg.mediaServer.rtspPort = uint16(mediaRTSPPort)
	if cfg.tokenTTL <= 0 {
		return config{}, fmt.Errorf("invalid token lifetime")
	}
	if cfg.mediaRequestTimeout <= 0 || cfg.inviteTimeout <= 0 || cfg.byeTimeout <= 0 {
		return config{}, fmt.Errorf("invalid live operation timeout")
	}
	return cfg, nil
}

func validDigits(value string, length int) bool {
	if len(value) != length {
		return false
	}
	for _, digit := range value {
		if digit < '0' || digit > '9' {
			return false
		}
	}
	return true
}
