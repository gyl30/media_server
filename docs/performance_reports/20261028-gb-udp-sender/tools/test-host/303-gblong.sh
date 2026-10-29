T=/var/tmp/ms-perf-891aec1
S="--perf-stat --control-probe-ms 200 --input rtmp --sources 1 --warmup 15 --ready-timeout 300 --ramp-per-second 200 --group-size 256"
for v in A B; do
RT=rt-gb$v $T/gbs.sh gb-l-rr-6400churn-$v-r1 --outputs gb_udp:6400 --gb-peer rr --churn gb_udp:100 --duration 900 $S
done
