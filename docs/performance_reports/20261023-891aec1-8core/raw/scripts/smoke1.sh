#!/bin/bash
T=/var/tmp/ms-perf-891aec1
for p in rtmp flv rtsp whep hls gb_tcp_passive gb_tcp_active gb_udp; do
  $T/pr.sh s1-$p-500 --input rtmp --sources 1 --outputs $p:500 --warmup 10 --duration 30 --ready-timeout 120 --ramp-per-second 200 --group-size 128
done
echo SMOKE1_DONE
