T=/var/tmp/ms-perf-891aec1
U="--upstream-workers 16 --perf-stat --warmup 15 --duration 60 --ready-timeout 300 --ramp-per-second 200 --group-size 256"
for n in 125 150; do
RT=rt-wp $T/wp.sh wq-mix$n-s13b --input whep_pull --sources $n --outputs flv:$n --pull-interleave-seed 13 $U
gzip $T/runs/d-wq-mix$n-s13b/result.json
done
for n in 160 170; do
RT=rt-wp $T/wp.sh wq-seq$n-r3 --input whep_pull --sources $n --outputs flv:$n $U
gzip $T/runs/d-wq-seq$n-r3/result.json
done
