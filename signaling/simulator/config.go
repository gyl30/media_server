package main

import (
	"flag"
	"fmt"
	"net"
	"net/netip"
	"net/url"
	"time"
)

type config struct {
	platformSIP       string
	controlURL        string
	listen            string
	platformID        string
	domain            string
	password          string
	deviceID          string
	channelID         string
	mediaFile         string
	mediaBind         string
	mediaSink         string
	mediaProfile      string
	ffmpeg            string
	registerExpiry    time.Duration
	heartbeat         time.Duration
	liveDuration      time.Duration
	devices           int
	liveCount         int
	sipEndpoints      int
	controlWorkers    int
	mediaWorkers      int
	phaseBuckets      int
	batchSize         int
	registerRate      int
	startRate         int
	packetLossPercent int
	seed              uint64
}

func parseConfig(args []string) (config, error) {
	var cfg config
	flags := flag.NewFlagSet("gb28181-simulator", flag.ContinueOnError)
	flags.StringVar(&cfg.platformSIP, "platform-sip", "127.0.0.1:5060", "GB28181 platform SIP UDP address")
	flags.StringVar(&cfg.controlURL, "control-url", "http://127.0.0.1:9090", "signaling internal HTTP base URL")
	flags.StringVar(&cfg.listen, "listen", "127.0.0.1:5062", "simulated device SIP UDP listen address")
	flags.StringVar(&cfg.platformID, "platform-id", "34020000002000000001", "GB28181 platform ID")
	flags.StringVar(&cfg.domain, "domain", "3402000000", "GB28181 SIP domain and Digest realm")
	flags.StringVar(&cfg.password, "password", "12345678", "device Digest password")
	flags.StringVar(&cfg.deviceID, "device-id", "34020000001320000001", "simulated device ID")
	flags.StringVar(&cfg.channelID, "channel-id", "34020000001320000002", "simulated channel ID")
	flags.StringVar(&cfg.mediaFile, "media-file", "", "Annex-B H264 input with AUD; empty generates a temporary fixture")
	flags.StringVar(&cfg.mediaBind, "media-bind", "0.0.0.0", "local IPv4 address for RTP sender sockets")
	flags.StringVar(&cfg.mediaSink, "media-sink", "", "generator-only RTP UDP sink as IPv4:port")
	flags.StringVar(&cfg.mediaProfile, "media-profile", "normal", "generated fixture bitrate profile: normal or high")
	flags.StringVar(&cfg.ffmpeg, "ffmpeg", "ffmpeg", "FFmpeg executable used to generate a temporary H264 fixture")
	flags.DurationVar(&cfg.registerExpiry, "register-expires", 120*time.Second, "REGISTER lifetime")
	flags.DurationVar(&cfg.heartbeat, "heartbeat", 30*time.Second, "Keepalive interval")
	flags.DurationVar(&cfg.liveDuration, "live-duration", 8*time.Second, "live duration")
	flags.IntVar(&cfg.devices, "devices", 1, "number of logical GB28181 devices")
	flags.IntVar(&cfg.liveCount, "live-count", 1, "number of live channels to start")
	flags.IntVar(&cfg.sipEndpoints, "sip-endpoints", 1, "shared SIP UDP endpoint shards")
	flags.IntVar(&cfg.controlWorkers, "control-workers", 16, "fixed SIP and HTTP control workers")
	flags.IntVar(&cfg.mediaWorkers, "media-workers", 16, "fixed RTP UDP workers and sockets")
	flags.IntVar(&cfg.phaseBuckets, "phase-buckets", 40, "RTP send phases per source frame")
	flags.IntVar(&cfg.batchSize, "batch-size", 64, "UDP sendmmsg batch size")
	flags.IntVar(&cfg.registerRate, "register-rate", 200, "maximum REGISTER transactions per second; zero sends a burst")
	flags.IntVar(&cfg.startRate, "start-rate", 0, "maximum live starts per second; zero sends a burst")
	flags.IntVar(&cfg.packetLossPercent, "packet-loss-percent", 0, "deterministic RTP packet loss percentage")
	flags.Uint64Var(&cfg.seed, "seed", 1, "deterministic fault injection seed")
	if err := flags.Parse(args); err != nil {
		return config{}, err
	}
	if flags.NArg() != 0 {
		return config{}, fmt.Errorf("unexpected argument %q", flags.Arg(0))
	}
	if _, err := net.ResolveUDPAddr("udp", cfg.platformSIP); err != nil {
		return config{}, fmt.Errorf("invalid platform SIP address: %w", err)
	}
	listen, err := net.ResolveUDPAddr("udp", cfg.listen)
	if err != nil || listen.IP == nil || listen.IP.IsUnspecified() {
		return config{}, fmt.Errorf("invalid device SIP listen address")
	}
	controlURL, err := url.Parse(cfg.controlURL)
	if err != nil || controlURL.Scheme != "http" || controlURL.Host == "" || controlURL.User != nil || (controlURL.Path != "" && controlURL.Path != "/") || controlURL.RawQuery != "" || controlURL.Fragment != "" {
		return config{}, fmt.Errorf("invalid signaling control URL")
	}
	if !validDigits(cfg.platformID, 20) || !validDigits(cfg.deviceID, 20) || !validDigits(cfg.channelID, 20) || !validDigits(cfg.domain, 10) {
		return config{}, fmt.Errorf("invalid GB28181 identity")
	}
	if cfg.password == "" {
		return config{}, fmt.Errorf("password is empty")
	}
	if address := net.ParseIP(cfg.mediaBind); address == nil || address.To4() == nil {
		return config{}, fmt.Errorf("invalid media bind address")
	}
	if cfg.mediaSink != "" {
		sink, err := netip.ParseAddrPort(cfg.mediaSink)
		if err != nil || !sink.Addr().Is4() || sink.Port() == 0 || cfg.liveCount == 0 {
			return config{}, fmt.Errorf("invalid media sink")
		}
	}
	if cfg.mediaProfile != "normal" && cfg.mediaProfile != "high" {
		return config{}, fmt.Errorf("invalid media profile")
	}
	if cfg.registerExpiry <= 0 || cfg.registerExpiry%time.Second != 0 {
		return config{}, fmt.Errorf("invalid registration lifetime")
	}
	if cfg.heartbeat <= 0 || cfg.heartbeat%time.Second != 0 || cfg.liveDuration <= 0 {
		return config{}, fmt.Errorf("invalid simulator duration")
	}
	if cfg.devices <= 0 || cfg.devices > 20_000 || cfg.liveCount < 0 || cfg.liveCount > cfg.devices {
		return config{}, fmt.Errorf("invalid simulator scale")
	}
	if cfg.sipEndpoints <= 0 || cfg.sipEndpoints > cfg.devices || cfg.controlWorkers <= 0 || cfg.mediaWorkers <= 0 || cfg.phaseBuckets <= 0 || cfg.phaseBuckets > 40 || cfg.batchSize <= 0 || cfg.batchSize > 128 || cfg.registerRate < 0 || cfg.startRate < 0 {
		return config{}, fmt.Errorf("invalid simulator worker configuration")
	}
	if cfg.packetLossPercent < 0 || cfg.packetLossPercent > 100 {
		return config{}, fmt.Errorf("invalid packet loss percentage")
	}
	if _, err := newIdentitySet(cfg.deviceID, cfg.channelID, cfg.devices); err != nil {
		return config{}, fmt.Errorf("invalid simulator identity range: %w", err)
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
