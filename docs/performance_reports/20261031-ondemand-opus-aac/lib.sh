T=/var/tmp/ms-perf-891aec1
R=$T/runs3
C="--perf-stat --control-probe-ms 200 --warmup 15 --duration 60 --ready-timeout 300 --ramp-per-second 200 --group-size 256 --cleanup-settle 5 --min-free-disk-mib 3072"
run() { name=$1; shift; $T/run3.sh $name "$@"; [ -f $R/$name/result.json ] && gzip -f $R/$name/result.json; }
traced() {
  name=$1; shift
  mkdir -p $R/trace
  bpftrace $T/transcoders.bt $T/${RT:-rt-r}/${SERVER_BIN:-build/media_server} > $R/trace/$name.bt 2>&1 &
  bt=$!
  sleep 3
  ( until [ -e $R/$name/measurement.start ]; do sleep 1; [ -d $R/$name ] || continue; done
    for p in $(pgrep -x media_server); do grep -q "198.19.0.10" /proc/$p/cmdline && pid=$p; done
    for t in /proc/$pid/task/*; do echo "$(basename $t) $(cat $t/comm)"; done > $R/trace/$name.tasks ) &
  watcher=$!
  run $name "$@"
  kill $watcher 2>/dev/null
  kill -INT $bt; wait $bt
}
