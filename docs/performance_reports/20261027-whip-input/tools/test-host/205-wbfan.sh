T=/var/tmp/ms-perf-891aec1
X=$T/fixtures-a1
H="--perf-stat --ready-timeout 240 --ramp-per-second 200 --group-size 256 --cleanup-settle 10 --control-probe-ms 200 --warmup 15 --duration 60"
for v in a3f wb; do
RT=rt-$v $T/pr2.sh wbux-whip80fan-$v-r1 --input whip --sources 80 --outputs flv:80,rtmp:80,rtsp:80,whep:80 --decode-probe $H
done
for v in wb a3f; do
RT=rt-$v $T/pr2.sh wbux-whip200fan-$v-r1 --input whip --sources 200 --outputs flv:200,rtmp:200,rtsp:200 --decode-probe $H
done
for v in a3f wb; do
RT=rt-$v $T/pr2.sh wbux-whip225av-$v-r1 --input whip --whip-fixture $X/avsync_whip.mkv --sources 225 --outputs flv:225 --av-probe 40 $H
done
