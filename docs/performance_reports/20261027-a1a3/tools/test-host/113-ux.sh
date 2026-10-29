T=/var/tmp/ms-perf-891aec1
X=$T/fixtures-a1
O="--sources 1 --outputs rtmp:2,flv:2,rtsp:2,hls:1,whep:2,gb_udp:2,gb_tcp_passive:2 --av-probe 40 --control-probe-ms 200 --gb-port-base 18700 --warmup 10 --duration 45 --cleanup-settle 5"
L="--input rtmp --fixture $X/avsync_720p30.flv --sources 1 --outputs rtmp:1000,flv:1000,rtsp:1000,whep:1000,gb_udp:1000,gb_tcp_passive:1000,hls:5 --av-probe 40 --control-probe-ms 200 --churn rtmp:50 --perf-stat --warmup 15 --duration 60 --ready-timeout 300 --ramp-per-second 200 --group-size 256 --cleanup-settle 10"
for input in rtmp rtsp_tcp whip whep_pull gb_udp gb_tcp_active; do
for v in final p1-base; do
fx="--fixture $X/avsync_720p30.flv"
[ $input = whip ] && fx="--whip-fixture $X/avsync_whip.mkv"
RT=rt-$v $T/pr2.sh ux-$input-${v#p1-}-r1 --input $input $fx $O
done
done
RT=rt-p1-base $T/pr2.sh ux-load6005-base-r1 $L
RT=rt-final $T/pr2.sh ux-load6005-final-r1 $L
RT=rt-final $T/pr2.sh ux-load6005-final-r2 $L
RT=rt-p1-base $T/pr2.sh ux-load6005-base-r2 $L
S="--perf-stat --control-probe-ms 200 --input rtmp --sources 1 --warmup 15 --ready-timeout 300 --ramp-per-second 200 --group-size 256"
M="rtmp:1250,flv:1250,rtsp:1250,whep:1250,gb_udp:1250,gb_tcp_passive:1250"
RT=rt-final $T/pr2.sh fin-rtmp9500-r1 --outputs rtmp:9500 --duration 60 $S
RT=rt-final $T/pr2.sh fin-mix7500-r1 --outputs $M --duration 60 $S
RT=rt-final $T/pr2.sh fin-gba12000-r1 --outputs gb_tcp_active:12000 --duration 60 --gb-port-base 18700 $S
RT=rt-final $T/pr2.sh fin-rtmp6500-r1 --outputs rtmp:6500 --duration 60 $S
RT=rt-p1-base $T/pr2.sh fin-rtmp6500-base-r1 --outputs rtmp:6500 --duration 60 $S
RT=rt-final $T/pr2.sh fin-slow60-r1 --input rtmp --sources 1 --outputs rtmp:1000,rtsp:1000,flv:1000,gb_tcp_passive:500 --stalled-viewers 20 --control-probe-ms 200 --warmup 10 --duration 60 --cleanup-settle 10
RT=rt-p1-base $T/pr2.sh fin-slow60-base-r1 --input rtmp --sources 1 --outputs rtmp:1000,rtsp:1000,flv:1000,gb_tcp_passive:500 --stalled-viewers 20 --control-probe-ms 200 --warmup 10 --duration 60 --cleanup-settle 10
RT=rt-final $T/pr2.sh fin-srcend-r1 --input rtmp --sources 4 --outputs rtmp:500,rtsp:500,flv:500,gb_tcp_passive:250 --fault source_stop --control-probe-ms 200 --warmup 10 --duration 40 --cleanup-settle 10
RT=rt-p1-base $T/pr2.sh fin-srcend-base-r1 --input rtmp --sources 4 --outputs rtmp:500,rtsp:500,flv:500,gb_tcp_passive:250 --fault source_stop --control-probe-ms 200 --warmup 10 --duration 40 --cleanup-settle 10
