T=/var/tmp/ms-perf-891aec1
S="--perf-stat --control-probe-ms 200 --input rtmp --sources 1 --warmup 15 --ready-timeout 300 --ramp-per-second 200 --group-size 256"
for spec in 7500:r1:AB 7500:r2:BA 8000:r1:AB; do
IFS=: read n r o <<< "$spec"
for v in ${o:0:1} ${o:1:1}; do
RT=rt-gb$v $T/gbs.sh gb-q-quiet-$n-$v-$r --outputs gb_udp:$n --gb-peer quiet --duration 120 $S
done
done
