#!/bin/bash
E=$(cd "$(dirname "$0")/.." && pwd); R=$(cd $E/../../.. && pwd)
P() { $E/scripts/run_perf.sh "$@" 2>&1; }
P ubsan $R/build-ubsan/media_server pf-ubsan-3cbc170-in-rtsp_tcp-r2 rtsp_tcp rtmp:4,rtsp:4,flv:4,whep:4,gb_udp:4,gb_tcp_passive:4 --duration 15
P asan $R/build-asan/media_server pf-asan-3cbc170-fault-source_stop-r2 rtmp rtmp:5,rtsp:5,flv:5,whep:5,gb_udp:5,gb_tcp_active:5 --fault source_stop
P tsan $R/build-tsan/media_server pf-tsan-3cbc170-in-gb_tcp_passive-r2 gb_tcp_passive rtmp:4,rtsp:4,flv:4,whep:4,gb_udp:4,gb_tcp_passive:4 --duration 15
for fault in invalid_token expired_token replayed_token; do
    for out in whep hls; do
        P asan $R/build-asan/media_server pf-asan-3cbc170-fault-$fault-$out rtmp $out:2 --fault $fault --duration 10
    done
done
for k in asan ubsan tsan; do
    $E/scripts/run_chrome.sh $k $R/build-$k/media_server chrome-$k-3cbc170 2>&1 | grep -E "^(PASS|FAIL)|failed|sanitizer_diagnostics|SUMMARY|runtime error"
    $E/scripts/run_pair.sh $k $R/build-$k/media_server talk-$k-3cbc170 talk 2>&1
    $E/scripts/run_pair.sh $k $R/build-$k/media_server whep_pull-$k-3cbc170 whep_pull 2>&1
done
