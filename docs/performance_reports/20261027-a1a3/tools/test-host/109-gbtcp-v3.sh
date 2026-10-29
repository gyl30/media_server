T=/var/tmp/ms-perf-891aec1
S="--perf-stat --control-probe-ms 200 --input rtmp --sources 1 --warmup 15 --ready-timeout 300 --ramp-per-second 200 --group-size 256"
G="--gb-port-base 18700 $S"
for n in 9500 10000 12000 13000; do
RT=rt-a2A-c999003 $T/pr2.sh v3-gba$n-pre-r1 --outputs gb_tcp_active:$n --duration 60 $G
RT=rt-cfe80a0 $T/pr2.sh v3-gba$n-a2-r1 --outputs gb_tcp_active:$n --duration 60 $G
RT=rt-a11gd $T/pr2.sh v3-gba$n-gd-r1 --outputs gb_tcp_active:$n --duration 60 $G
RT=rt-a11gd $T/pr2.sh v3-gbp$n-gd-r1 --outputs gb_tcp_passive:$n --duration 60 $S
RT=rt-cfe80a0 $T/pr2.sh v3-gbp$n-a2-r1 --outputs gb_tcp_passive:$n --duration 60 $S
RT=rt-a2A-c999003 $T/pr2.sh v3-gbp$n-pre-r1 --outputs gb_tcp_passive:$n --duration 60 $S
done
