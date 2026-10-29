T=/var/tmp/ms-perf-891aec1
U="--perf-stat --warmup 15 --duration 60 --ready-timeout 240 --ramp-per-second 200 --group-size 256"
for n in 100 150 200 250; do
RT=rt-a3f $T/wd.sh wd-whip$n-r1 --input whip --sources $n --outputs flv:$n $U
done
RT=rt-a3f $T/prof3.sh whip150-final --input whip --sources 150 --outputs flv:150 --perf-stat --ready-timeout 240 --ramp-per-second 200 --group-size 256
