T=/var/tmp/ms-perf-891aec1
U="--upstream-workers 16 --perf-stat --control-probe-ms 200 --warmup 15 --duration 60 --ready-timeout 300 --ramp-per-second 200 --group-size 64 --upstream-sources 64 --min-free-disk-mib 3072"
run() { rt=$1; name=$2; shift 2; RT=$rt $T/pr2.sh $name "$@" $U; gzip $T/runs/$name/result.json; }
run rt-fdk e-F-seq320-u64 --input whep_pull --sources 320 --outputs flv:320
run rt-fdk e-F-seq340-u64 --input whep_pull --sources 340 --outputs flv:340
run rt-fdk e-F-mix300-s12-u64 --input whep_pull --sources 300 --outputs flv:300 --pull-interleave-seed 12
run rt-ls f-L-seq168-u64 --input whep_pull --sources 168 --outputs flv:168
