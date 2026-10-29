T=/var/tmp/ms-perf-891aec1
U="--perf-stat --warmup 15 --duration 60 --ready-timeout 240 --ramp-per-second 200 --group-size 256"
for n in 200 250; do
RT=rt-wb $T/wd.sh wd-wb-whip$n-r1 --input whip --sources $n --outputs flv:$n $U
done
RT=rt-a3f $T/wd.sh wd-whip200-r2 --input whip --sources 200 --outputs flv:200 $U
