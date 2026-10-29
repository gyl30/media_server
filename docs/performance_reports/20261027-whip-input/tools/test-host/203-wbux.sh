T=/var/tmp/ms-perf-891aec1
X=$T/fixtures-a1
O="--sources 1 --outputs rtmp:2,flv:2,rtsp:2,hls:1,whep:2,gb_udp:2,gb_tcp_passive:2 --av-probe 40 --control-probe-ms 200 --gb-port-base 18700 --warmup 10 --duration 45 --cleanup-settle 5"
H="--perf-stat --ready-timeout 240 --ramp-per-second 200 --group-size 256 --cleanup-settle 10 --control-probe-ms 200"
for v in wb a3f; do
RT=rt-$v $T/pr2.sh wbux-whip1-$v-r1 --input whip --whip-fixture $X/avsync_whip.mkv $O
done
for v in a3f wb; do
RT=rt-$v $T/pr2.sh wbux-whip200av-$v-r1 --input whip --whip-fixture $X/avsync_whip.mkv --sources 200 --outputs flv:200 --av-probe 40 --decode-probe --warmup 15 --duration 60 $H
done
for v in wb a3f; do
RT=rt-$v $T/pr2.sh wbux-whip150fan-$v-r1 --input whip --sources 150 --outputs flv:150,rtmp:150,rtsp:150,whep:150 --decode-probe --warmup 15 --duration 60 $H
done
for v in a3f wb; do
RT=rt-$v $T/pr2.sh wbux-whip100stop-$v-r1 --input whip --sources 100 --outputs flv:100,rtsp:100 --fault source_stop --warmup 10 --duration 60 $H
done
for v in wb a3f; do
RT=rt-$v $T/pr2.sh wbux-whip100churn-$v-r1 --input whip --sources 100 --outputs flv:100 --churn flv:50 --warmup 15 --duration 60 $H
done
