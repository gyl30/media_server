T=/var/tmp/ms-perf-891aec1
U="--perf-stat --warmup 15 --duration 60 --ready-timeout 240 --ramp-per-second 200 --group-size 256"
for r in 1 2; do
for n in 265 280; do
RT=rt-wb $T/a3.sh wb-whip$n-wb-r$r --input whip --sources $n --outputs flv:$n $U
done
done
