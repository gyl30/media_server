#!/bin/bash
# B2 sanitizer regression: lifecycle (tsan, asan, ubsan) and perf_run mixed / multi-source (tsan, asan)
E=$(cd "$(dirname "$0")" && pwd); R=$(cd $E/../../../.. && pwd)
for k in tsan asan ubsan; do
    $E/scripts/run_lifecycle.sh $k $R/build-$k/media_server lc-$k-B2 | tail -c 600; echo
done
for k in tsan asan; do
    $E/scripts/run_perf.sh $k $R/build-$k/media_server pf-$k-B2-mixed rtmp rtmp:10,rtsp:10,flv:10,hls:10,whep:10,gb_udp:10,gb_tcp_passive:10,gb_tcp_active:10
    $E/scripts/run_perf.sh $k $R/build-$k/media_server pf-$k-B2-src4 rtmp rtmp:40,rtsp:20,flv:20,whep:20,gb_udp:40 --sources 4 --churn rtmp:10 --churn-period 12 --duration 40
done
