T=/var/tmp/ms-perf-891aec1
U="--upstream-workers 16 --perf-stat --control-probe-ms 200 --warmup 15 --duration 60 --ready-timeout 300 --ramp-per-second 200 --group-size 256 --min-free-disk-mib 3584"
run() { rt=$1; name=$2; shift 2; RT=$rt $T/pr2.sh $name "$@" $U; gzip $T/runs/$name/result.json; }
run rt-pow e-P-seq176 --input whep_pull --sources 176 --outputs flv:176
run rt-pow e-P-seq184 --input whep_pull --sources 184 --outputs flv:184
run rt-pow e-P-mix176-s11 --input whep_pull --sources 176 --outputs flv:176 --pull-interleave-seed 11
run rt-ls f-L-seq168 --input whep_pull --sources 168 --outputs flv:168
run rt-pow e-P-seq168 --input whep_pull --sources 168 --outputs flv:168
