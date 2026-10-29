#!/bin/bash
T=/var/tmp/ms-perf-891aec1
ALL=rtmp:20,rtsp:20,flv:20,hls:5,whep:20,gb_udp:20,gb_tcp_active:20,gb_tcp_passive:20
COMMON="--sources 1 --warmup 10 --duration 20 --ready-timeout 120 --ramp-per-second 100 --group-size 64 --decode-probe"
SERVER_CPUS=0 WORKERS=1 $T/pr.sh v4-neg-rtmp-1cpu-2000 --input rtmp --sources 1 --outputs rtmp:2000 --warmup 10 --duration 30 --ready-timeout 240 --ramp-per-second 100 --group-size 256
for i in rtmp rtsp_tcp rtsp_udp rtsp_pull rtmp_pull whep_pull whip gb_udp gb_tcp_active gb_tcp_passive; do
  $T/pr.sh v4-in-$i --input $i --outputs $ALL $COMMON
done
for f in h265_aac h264_pcma h264_pcmu h265_pcma; do
  $T/pr.sh v4-codec-$f-rtmp --input rtmp --fixture .cache/$f.mkv --outputs $ALL $COMMON
done
$T/pr.sh v4-codec-h265_aac-rtsp --input rtsp_tcp --fixture .cache/h265_aac.mkv --outputs $ALL $COMMON
$T/pr.sh v4-codec-h264_opus-rtsp --input rtsp_tcp --fixture .cache/perf_fixture_whip.mkv --outputs $ALL $COMMON
$T/pr.sh v4-codec-h264_opus-rtmp --input rtmp --fixture .cache/perf_fixture_whip.mkv --outputs $ALL $COMMON
echo VAL4_DONE
