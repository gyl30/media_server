#!/bin/bash
T=/var/tmp/ms-perf-891aec1
$T/pr.sh v1-rtmp-500 --input rtmp --sources 1 --outputs rtmp:500 --warmup 10 --duration 30 --ready-timeout 120 --ramp-per-second 200 --group-size 128
$T/pr.sh v1-whep-200 --input rtmp --sources 1 --outputs whep:200 --warmup 10 --duration 30 --ready-timeout 120 --ramp-per-second 200 --group-size 128
$T/pr.sh v1-rtsp-200 --input rtmp --sources 1 --outputs rtsp:200 --warmup 10 --duration 30 --ready-timeout 120 --ramp-per-second 200 --group-size 128
$T/pr.sh v1-flv-200 --input rtmp --sources 1 --outputs flv:200 --warmup 10 --duration 30 --ready-timeout 120 --ramp-per-second 200 --group-size 128
sed -i 's/--server-cpus 0-7/--server-cpus ${SERVER_CPUS:-0-7}/; s/--workers 8/--workers ${WORKERS:-8}/' $T/pr.sh
SERVER_CPUS=0 WORKERS=1 $T/pr.sh v1-neg-rtmp-1cpu-2000 --input rtmp --sources 1 --outputs rtmp:2000 --warmup 10 --duration 30 --ready-timeout 180 --ramp-per-second 200 --group-size 250
echo VAL1_DONE
