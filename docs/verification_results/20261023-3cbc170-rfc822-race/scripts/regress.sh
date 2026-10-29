#!/bin/bash
E=$(cd "$(dirname "$0")/.." && pwd); R=$(cd $E/../../.. && pwd)
for k in asan ubsan; do
    $E/scripts/run_date.sh $k $R/build-$k/media_server date-$k-fix
done
$E/scripts/run_date.sh none $R/build/media_server date-release-fix
$E/scripts/run_date.sh none $E/before-3cbc170/media_server-release date-release-3cbc170
for k in tsan asan none; do
    bin=$R/build-$k/media_server; [ $k = none ] && bin=$R/build/media_server
    tag=$k; [ $k = none ] && tag=release
    $E/scripts/run_lifecycle.sh $k $bin lc-$tag-fix | tail -c 900; echo
    for input in rtsp_tcp rtsp_udp; do
        $E/scripts/run_perf.sh $k $bin pf-$tag-fix-in-$input $input rtmp:4,rtsp:4,flv:4,whep:4,gb_udp:4,gb_tcp_passive:4 --duration 15
    done
    $E/scripts/run_perf.sh $k $bin pf-$tag-fix-mixed rtmp rtmp:10,rtsp:10,flv:10,hls:10,whep:10,gb_udp:10,gb_tcp_passive:10,gb_tcp_active:10
done
$E/scripts/run_lifecycle.sh ubsan $R/build-ubsan/media_server lc-ubsan-fix | tail -c 900; echo
