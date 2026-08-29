package main

import (
	"flag"
	"fmt"
	"io"
	"net"
	"time"
)

type config struct {
	sipListen           string
	sipAdvertise        string
	httpListen          string
	sipID               string
	sipDomain           string
	sipPassword         string
	registerExpires     time.Duration
	heartbeatTimeout    time.Duration
	mediaServerTimeout  time.Duration
	mediaRequestTimeout time.Duration
	inviteTimeout       time.Duration
	byeTimeout          time.Duration
}

func parseConfig(args []string) (config, error) {
	var cfg config
	flags := flag.NewFlagSet("signaling", flag.ContinueOnError)
	flags.SetOutput(io.Discard)
	flags.StringVar(&cfg.sipListen, "sip-listen", "0.0.0.0:5060", "SIP UDP listen address")
	flags.StringVar(&cfg.sipAdvertise, "sip-advertise", "127.0.0.1:5060", "SIP address advertised to devices")
	flags.StringVar(&cfg.httpListen, "http-listen", "127.0.0.1:9090", "internal HTTP listen address")
	flags.StringVar(&cfg.sipID, "sip-id", "34020000002000000001", "GB28181 platform ID")
	flags.StringVar(&cfg.sipDomain, "sip-domain", "3402000000", "GB28181 SIP domain and Digest realm")
	flags.StringVar(&cfg.sipPassword, "sip-password", "12345678", "shared GB28181 device password")
	flags.DurationVar(&cfg.registerExpires, "register-expires", time.Hour, "default registration lifetime")
	flags.DurationVar(&cfg.heartbeatTimeout, "heartbeat-timeout", 90*time.Second, "device heartbeat timeout")
	flags.DurationVar(&cfg.mediaServerTimeout, "media-server-timeout", 15*time.Second, "media server heartbeat timeout")
	flags.DurationVar(&cfg.mediaRequestTimeout, "media-request-timeout", 3*time.Second, "media server HTTP request timeout")
	flags.DurationVar(&cfg.inviteTimeout, "invite-timeout", 10*time.Second, "live INVITE timeout")
	flags.DurationVar(&cfg.byeTimeout, "bye-timeout", 3*time.Second, "live BYE timeout")
	if err := flags.Parse(args); err != nil {
		return config{}, err
	}
	if flags.NArg() != 0 {
		return config{}, fmt.Errorf("unexpected argument %q", flags.Arg(0))
	}
	if _, err := net.ResolveUDPAddr("udp", cfg.sipListen); err != nil {
		return config{}, fmt.Errorf("invalid SIP listen address: %w", err)
	}
	if _, err := net.ResolveTCPAddr("tcp", cfg.httpListen); err != nil {
		return config{}, fmt.Errorf("invalid HTTP listen address: %w", err)
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
	if cfg.mediaServerTimeout <= 0 {
		return config{}, fmt.Errorf("invalid media server timeout")
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
