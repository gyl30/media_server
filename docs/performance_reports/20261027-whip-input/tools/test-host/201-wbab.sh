T=/var/tmp/ms-perf-891aec1
U="--perf-stat --warmup 15 --duration 60 --ready-timeout 240 --ramp-per-second 200 --group-size 256"
for r in 1 2 3; do
for n in 175 200 225 250; do
if [ $((r % 2)) = 1 ]; then order="a3f wb"; else order="wb a3f"; fi
for v in $order; do
RT=rt-$v $T/a3.sh wb-whip$n-$v-r$r --input whip --sources $n --outputs flv:$n $U
done
done
done
