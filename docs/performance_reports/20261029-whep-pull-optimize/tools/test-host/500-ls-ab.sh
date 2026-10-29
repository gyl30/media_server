T=/var/tmp/ms-perf-891aec1
U="--upstream-workers 16 --perf-stat --control-probe-ms 200 --warmup 15 --duration 60 --ready-timeout 300 --ramp-per-second 200 --group-size 256 --min-free-disk-mib 3584"
diag() { rt=$1; name=$2; shift 2; RT=$rt $T/wp.sh $name "$@" $U; gzip $T/runs/d-$name/result.json; }
for s in 11 12 13; do
diag rt-A x-A-mix150-s$s --input whep_pull --sources 150 --outputs flv:150 --pull-interleave-seed $s
diag rt-ls x-L-mix150-s$s --input whep_pull --sources 150 --outputs flv:150 --pull-interleave-seed $s
done
diag rt-ls x-L-mix160-s11 --input whep_pull --sources 160 --outputs flv:160 --pull-interleave-seed 11
diag rt-ls x-L-mix160-s12 --input whep_pull --sources 160 --outputs flv:160 --pull-interleave-seed 12
diag rt-ls x-L-seq170 --input whep_pull --sources 170 --outputs flv:170
