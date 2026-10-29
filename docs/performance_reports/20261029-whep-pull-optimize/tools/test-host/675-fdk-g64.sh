T=/var/tmp/ms-perf-891aec1
U="--upstream-workers 16 --perf-stat --control-probe-ms 200 --warmup 15 --duration 60 --ready-timeout 300 --ramp-per-second 200 --group-size 64 --min-free-disk-mib 3072"
run() { rt=$1; name=$2; shift 2; RT=$rt $T/pr2.sh $name "$@" $U; gzip $T/runs/$name/result.json; }
run rt-fdk e-F-seq250-g64 --input whep_pull --sources 250 --outputs flv:250
run rt-fdk e-F-seq300-g64 --input whep_pull --sources 300 --outputs flv:300
run rt-pow e-P-seq200-g64 --input whep_pull --sources 200 --outputs flv:200
