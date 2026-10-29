#!/bin/bash
T=/var/tmp/ms-perf-891aec1
for p in gb_udp gb_tcp_active gb_tcp_passive; do
  $T/pr.sh v2-$p-200 --input rtmp --sources 1 --outputs $p:200 --warmup 10 --duration 30 --ready-timeout 120 --ramp-per-second 200 --group-size 128
done
$T/pr.sh v2-gb_udp-500 --input rtmp --sources 1 --outputs gb_udp:500 --warmup 10 --duration 30 --ready-timeout 120 --ramp-per-second 200 --group-size 256
$T/pr.sh v2-gb_tcp_passive-500 --input rtmp --sources 1 --outputs gb_tcp_passive:500 --warmup 10 --duration 30 --ready-timeout 120 --ramp-per-second 200 --group-size 256
for n in 600 900 1200; do
  SERVER_CPUS=0 WORKERS=1 $T/pr.sh v2-neg-rtmp-1cpu-$n --input rtmp --sources 1 --outputs rtmp:$n --warmup 10 --duration 30 --ready-timeout 120 --ramp-per-second 100 --group-size 256
done
echo VAL2_DONE
