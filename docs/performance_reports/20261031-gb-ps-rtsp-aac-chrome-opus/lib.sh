T=/var/tmp/ms-perf-891aec1
R=$T/runs4
C="--perf-stat --control-probe-ms 200 --warmup 15 --duration 60 --ready-timeout 300 --ramp-per-second 200 --group-size 256 --cleanup-settle 5 --min-free-disk-mib 3072"
CHROME="minptime=10;useinbandfec=1"
run() { name=$1; shift; $T/run4.sh $name "$@"; [ -f $R/$name/result.json ] && gzip -f $R/$name/result.json; }
st() { python3 $T/st.py $R/$1 | cut -d" " -f1; }
traced() {
  name=$1; shift
  mkdir -p $R/trace
  bpftrace $T/transcoders.bt $T/${RT:-rt-s}/${SERVER_BIN:-build/media_server} > $R/trace/$name.bt 2>&1 &
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
search() {
  variant=$1; bin=$2; s=$3; step=$4; limit=$5
  export SERVER_BIN=$bin
  pass=0; fail=0
  while [ $s -le $limit ]; do
    run cap-chrome-$variant-$s-r1 --input whip --sources $s --outputs whep:$((2 * s)) --group-size 128 --whep-opus-fmtp "$CHROME" $C
    if [ "$(st cap-chrome-$variant-$s-r1)" = PASS ]; then pass=$s; s=$((s + step)); else fail=$s; break; fi
  done
  if [ $fail -gt 0 ] && [ $pass -gt 0 ] && [ $((fail - pass)) -gt 32 ]; then
    m=$(( (pass + fail) / 2 ))
    run cap-chrome-$variant-$m-r1 --input whip --sources $m --outputs whep:$((2 * m)) --group-size 128 --whep-opus-fmtp "$CHROME" $C
    [ "$(st cap-chrome-$variant-$m-r1)" = PASS ] && pass=$m || fail=$m
  fi
  [ $pass -gt 0 ] && run cap-chrome-$variant-$pass-r2 --input whip --sources $pass --outputs whep:$((2 * pass)) --group-size 128 --whep-opus-fmtp "$CHROME" $C
  echo "SEARCH chrome-$variant pass=$pass fail=$fail r2=$(st cap-chrome-$variant-$pass-r2)"
  for d in $R/cap-chrome-$variant-*; do [ -d $d ] && echo "$(basename $d) $(python3 $T/st.py $d)"; done
  unset SERVER_BIN
}
