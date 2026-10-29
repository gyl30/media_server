T=/var/tmp/ms-perf-891aec1
U="--upstream-workers 16 --perf-stat --control-probe-ms 200 --warmup 15 --duration 60 --ready-timeout 300 --ramp-per-second 200 --group-size 256"
for n in 25 50 75 100 125 150 160 170 180 190; do
RT=rt-wp $T/pr2.sh wq-cap$n-seq-r1 --input whep_pull --sources $n --outputs flv:$n $U
done
