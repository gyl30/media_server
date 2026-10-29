T=/var/tmp/ms-perf-891aec1
U="--upstream-workers 16 --perf-stat --warmup 15 --duration 60 --ready-timeout 300 --ramp-per-second 200 --group-size 256"
for s in 11 12 13; do
for n in 125 150; do
RT=rt-wp $T/wp.sh wq-mix$n-s$s --input whep_pull --sources $n --outputs flv:$n --pull-interleave-seed $s $U
done
done
for n in 160 170; do
RT=rt-wp $T/wp.sh wq-seq$n-r2 --input whep_pull --sources $n --outputs flv:$n $U
done
