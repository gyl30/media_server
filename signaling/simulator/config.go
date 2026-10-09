package main

import (
	"flag"
	"fmt"
	"net"
	"net/netip"
	"time"
)

type config struct {
	platformSIP       string
	listen            string
	platformID        string
	domain            string
	password          string
	deviceID          string
	channelID         string
	mediaFile         string
	audioFile         string
	mediaBind         string
	mediaSink         string
	mediaProfile      string
	ffmpeg            string
	registerExpiry    time.Duration
	heartbeat         time.Duration
	duration          time.Duration
	devices           int
	sipEndpoints      int
	controlWorkers    int
	mediaWorkers      int
	phaseBuckets      int
	batchSize         int
	registerRate      int
	packetLossPercent int
	seed              uint64
	answerSSRC        uint
}

func parseConfig(args []string) (config, error) {
	var cfg config
	flags := flag.NewFlagSet("gb28181-simulator", flag.ContinueOnError)
	flags.StringVar(&cfg.platformSIP, "platform-sip", "127.0.0.1:5060", "GB28181 platform SIP UDP address")
	flags.StringVar(&cfg.listen, "listen", "127.0.0.1:5062", "simulated device SIP UDP listen address")
	flags.StringVar(&cfg.platformID, "platform-id", "34020000002000000001", "GB28181 platform ID")
	flags.StringVar(&cfg.domain, "domain", "3402000000", "GB28181 SIP domain and Digest realm")
	flags.StringVar(&cfg.password, "password", "12345678", "device Digest password")
	flags.StringVar(&cfg.deviceID, "device-id", "34020000001320000001", "simulated device ID")
	flags.StringVar(&cfg.channelID, "channel-id", "34020000001320000002", "simulated channel ID")
	flags.StringVar(&cfg.mediaFile, "media-file", "", "Annex-B H264 input with AUD; empty generates a temporary fixture")
	flags.StringVar(&cfg.audioFile, "audio-file", "", "optional raw PCMA input: 8000 Hz mono, nonempty multiple of 320 bytes; looped in 40 ms packets")
	flags.StringVar(&cfg.mediaBind, "media-bind", "127.0.0.1", "local IPv4 address for RTP sender sockets")
	flags.StringVar(&cfg.mediaSink, "media-sink", "", "generator-only RTP UDP sink as IPv4:port")
	flags.StringVar(&cfg.mediaProfile, "media-profile", "normal", "generated fixture bitrate profile: normal or high")
	flags.StringVar(&cfg.ffmpeg, "ffmpeg", "ffmpeg", "FFmpeg executable used to generate a temporary H264 fixture")
	flags.DurationVar(&cfg.registerExpiry, "register-expires", 120*time.Second, "REGISTER lifetime")
	flags.DurationVar(&cfg.heartbeat, "heartbeat", 30*time.Second, "Keepalive interval")
	flags.DurationVar(&cfg.duration, "duration", 8*time.Second, "simulator runtime")
	flags.IntVar(&cfg.devices, "devices", 1, "number of logical GB28181 devices")
	flags.IntVar(&cfg.sipEndpoints, "sip-endpoints", 1, "shared SIP UDP endpoint shards")
	flags.IntVar(&cfg.controlWorkers, "control-workers", 16, "fixed SIP control workers")
	flags.IntVar(&cfg.mediaWorkers, "media-workers", 16, "fixed RTP UDP workers and sockets")
	flags.IntVar(&cfg.phaseBuckets, "phase-buckets", 40, "RTP send phases per source frame")
	flags.IntVar(&cfg.batchSize, "batch-size", 64, "UDP sendmmsg batch size")
	flags.IntVar(&cfg.registerRate, "register-rate", 200, "maximum REGISTER transactions per second; zero sends a burst")
	flags.IntVar(&cfg.packetLossPercent, "packet-loss-percent", 0, "deterministic RTP packet loss percentage")
	flags.Uint64Var(&cfg.seed, "seed", 1, "deterministic fault injection seed")
	flags.UintVar(&cfg.answerSSRC, "answer-ssrc", 0, "device-selected SSRC returned in INVITE 200 OK y= and used for RTP; 0 echoes the platform SSRC")
	if err := flags.Parse(args); err != nil {
		return config{}, err
	}
	if flags.NArg() != 0 {
		return config{}, fmt.Errorf("unexpected argument %q", flags.Arg(0))
	}
	if _, err := net.ResolveUDPAddr("udp", cfg.platformSIP); err != nil {
		return config{}, fmt.Errorf("invalid platform SIP address: %w", err)
	}
	listen, err := netip.ParseAddrPort(cfg.listen)
	if err != nil || listen.Addr().IsUnspecified() {
		return config{}, fmt.Errorf("invalid device SIP listen address")
	}
	if !validDigits(cfg.platformID, 20) || !validDigits(cfg.deviceID, 20) || !validDigits(cfg.channelID, 20) || !validDigits(cfg.domain, 10) {
		return config{}, fmt.Errorf("invalid GB28181 identity")
	}
	if cfg.password == "" {
		return config{}, fmt.Errorf("password is empty")
	}
	if address := net.ParseIP(cfg.mediaBind); address == nil || address.To4() == nil || address.IsUnspecified() {
		return config{}, fmt.Errorf("invalid media bind address")
	}
	if cfg.mediaSink != "" {
		sink, err := netip.ParseAddrPort(cfg.mediaSink)
		if err != nil || !sink.Addr().Is4() || sink.Port() == 0 {
			return config{}, fmt.Errorf("invalid media sink")
		}
	}
	if cfg.mediaProfile != "normal" && cfg.mediaProfile != "high" {
		return config{}, fmt.Errorf("invalid media profile")
	}
	if cfg.answerSSRC > 0xffffffff {
		return config{}, fmt.Errorf("invalid answer SSRC")
	}
	if cfg.registerExpiry <= 0 || cfg.registerExpiry%time.Second != 0 {
		return config{}, fmt.Errorf("invalid registration lifetime")
	}
	if cfg.heartbeat <= 0 || cfg.heartbeat%time.Second != 0 || cfg.duration <= 0 {
		return config{}, fmt.Errorf("invalid simulator duration")
	}
	if cfg.devices <= 0 || cfg.devices > 20_000 {
		return config{}, fmt.Errorf("invalid simulator scale")
	}
	if cfg.sipEndpoints <= 0 || cfg.sipEndpoints > cfg.devices || cfg.controlWorkers <= 0 || cfg.mediaWorkers <= 0 || cfg.phaseBuckets <= 0 || cfg.phaseBuckets > 40 || cfg.batchSize <= 0 || cfg.batchSize > 128 || cfg.registerRate < 0 {
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
