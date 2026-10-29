T=/var/tmp/ms-perf-891aec1
S="--perf-stat --control-probe-ms 200 --input rtmp --sources 1 --warmup 15 --ready-timeout 300 --ramp-per-second 200 --group-size 256"
i=0
for m in quiet rtcp rtcp_rr rr rtp_noise; do
if [ $((i % 2)) = 0 ]; then order="A B"; else order="B A"; fi
for v in $order; do
RT=rt-gb$v $T/gbs.sh gb-p-$m-1000-$v-r1 --outputs gb_udp:1000 --gb-peer $m --duration 300 $S
done
i=$((i + 1))
done
