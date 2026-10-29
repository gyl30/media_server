#!/bin/bash
# usage: matrix.sh <asan|ubsan|tsan|none> <media_server-bin> <tag>
E=$(cd "$(dirname "$0")/.." && pwd); kind=$1; bin=$2; tag=$3
run() { $E/scripts/run_perf.sh $kind "$bin" "$@" 2>&1; }
run pf-$tag-mixed rtmp rtmp:10,rtsp:10,flv:10,hls:10,whep:10,gb_udp:10,gb_tcp_passive:10,gb_tcp_active:10
for input in rtsp_tcp rtsp_udp rtsp_pull whip gb_udp gb_tcp_active gb_tcp_passive; do
    run pf-$tag-in-$input $input rtmp:4,rtsp:4,flv:4,whep:4,gb_udp:4,gb_tcp_passive:4 --duration 15
done
run pf-$tag-fault-source_stop rtmp rtmp:5,rtsp:5,flv:5,whep:5,gb_udp:5,gb_tcp_active:5 --fault source_stop
for fault in invalid_token expired_token replayed_token; do
    run pf-$tag-fault-$fault rtmp rtmp:2,rtsp:2,flv:2,whep:2 --fault $fault --duration 10
done
