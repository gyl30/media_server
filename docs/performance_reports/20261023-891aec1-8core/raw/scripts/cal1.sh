#!/bin/bash
T=/var/tmp/ms-perf-891aec1
C="--input rtmp --sources 1 --warmup 15 --duration 30 --ready-timeout 180 --ramp-per-second 200 --group-size 256"
$T/pr.sh c1-rtmp-3000 $C --outputs rtmp:3000
$T/pr.sh c1-flv-3000 $C --outputs flv:3000
$T/pr.sh c1-rtsp-3000 $C --outputs rtsp:3000
$T/pr.sh c1-whep-1000 $C --outputs whep:1000
$T/pr.sh c1-gb_udp-2000 $C --outputs gb_udp:2000
$T/pr.sh c1-gb_tcp_passive-3000 $C --outputs gb_tcp_passive:3000
echo CAL1_DONE
