#!/bin/bash
T=/var/tmp/ms-perf-891aec1
SERVER_CPUS=0 WORKERS=1 $T/pr.sh v5-neg-rtmp-1cpu-2000 --input rtmp --sources 1 --outputs rtmp:2000 --warmup 10 --duration 30 --ready-timeout 240 --ramp-per-second 100 --group-size 256
$T/pr.sh v5-in-whep_pull --input whep_pull --sources 1 --outputs rtmp:20,flv:20,rtsp:20 --warmup 10 --duration 20 --ready-timeout 120 --ramp-per-second 100 --group-size 64
echo VAL5_DONE
