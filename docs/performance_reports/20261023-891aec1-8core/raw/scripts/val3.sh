#!/bin/bash
T=/var/tmp/ms-perf-891aec1
$T/pr.sh v3-flv-500 --input rtmp --sources 1 --outputs flv:500 --warmup 10 --duration 30 --ready-timeout 120 --ramp-per-second 200 --group-size 128
for n in 1600 2000; do
  SERVER_CPUS=0 WORKERS=1 $T/pr.sh v3-neg-rtmp-1cpu-$n --input rtmp --sources 1 --outputs rtmp:$n --warmup 10 --duration 30 --ready-timeout 240 --ramp-per-second 100 --group-size 256
done
echo VAL3_DONE
