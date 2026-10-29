T=/var/tmp/ms-perf-891aec1
U="--upstream-workers 16 --perf-stat --control-probe-ms 200 --warmup 15 --duration 60 --ready-timeout 300 --ramp-per-second 200 --group-size 256 --min-free-disk-mib 3584"
run() { rt=$1; name=$2; shift 2; RT=$rt $T/pr2.sh $name "$@" $U; gzip $T/runs/$name/result.json; }
for s in 11 12 13; do
run rt-A f-A-mix150-s$s --input whep_pull --sources 150 --outputs flv:150 --pull-interleave-seed $s
run rt-ls f-L-mix150-s$s --input whep_pull --sources 150 --outputs flv:150 --pull-interleave-seed $s
done
run rt-ls f-L-mix165-s11 --input whep_pull --sources 165 --outputs flv:165 --pull-interleave-seed 11
run rt-A f-A-seq160 --input whep_pull --sources 160 --outputs flv:160
run rt-ls f-L-seq160 --input whep_pull --sources 160 --outputs flv:160
