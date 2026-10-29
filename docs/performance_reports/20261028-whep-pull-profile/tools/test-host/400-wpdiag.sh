T=/var/tmp/ms-perf-891aec1
U="--perf-stat --warmup 15 --duration 60 --ready-timeout 300 --ramp-per-second 200 --group-size 256"
for n in 100 150 200; do
RT=rt-wp $T/wp.sh wp-seq$n-r1 --input whep_pull --sources $n --outputs flv:$n $U
done
RT=rt-wp $T/wp.sh wp-mix150-s1 --input whep_pull --sources 150 --outputs flv:150 --pull-interleave-seed 1 $U
RT=rt-wp $T/wp.sh wp-mix200-s2 --input whep_pull --sources 200 --outputs flv:200 --pull-interleave-seed 2 $U
