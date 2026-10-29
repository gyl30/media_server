T=/var/tmp/ms-perf-891aec1
U="--upstream-workers 16 --perf-stat --warmup 15 --duration 60 --ready-timeout 300 --ramp-per-second 200 --group-size 256"
for n in 150 200 250; do
RT=rt-wp $T/wp.sh wq-seq$n-r1 --input whep_pull --sources $n --outputs flv:$n $U
done
RT=rt-wp $T/wp.sh wq-mix200-s3 --input whep_pull --sources 200 --outputs flv:200 --pull-interleave-seed 3 $U
