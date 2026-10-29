T=/var/tmp/ms-perf-891aec1
S="--perf-stat --control-probe-ms 200 --input rtmp --sources 1 --warmup 15 --ready-timeout 300 --ramp-per-second 200 --group-size 256"
for n in 1400 1500; do
M="rtmp:$n,flv:$n,rtsp:$n,whep:$n,gb_udp:$n,gb_tcp_passive:$n"
RT=rt-final $T/pr2.sh cap-mix$((n*6))-FIN-r1 --outputs $M --duration 60 $S
RT=rt-cfe80a0 $T/pr2.sh cap-mix$((n*6))-A-r1 --outputs $M --duration 60 $S
done
