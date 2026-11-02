. /var/tmp/ms-perf-891aec1/lib4.sh
NS=msperf-20261013T011210Z-7788320
R5=$T/runs5
mkdir -p $R5
for input in whip whep_pull; do
  for variant in a b; do
    name=q-join-$input-$variant
    rm -rf $R5/$name
    bash $T/env_check.sh $R5/$name.env.json > /dev/null || { echo "$name BLOCKED"; continue; }
    ip netns exec $NS python3 $T/aac_join.py --output $R5/$name --runtime $T/rt-s --server-bin build-t/media_server-$variant --input $input --expect-restarts 1 > $R5/$name.out 2>&1
    echo "== $name exit=$?"; tail -2 $R5/$name.out
  done
done
for san in asan tsan; do
  mkdir -p $R5/q-san-$san-log
  opts="log_path=$R5/q-san-$san-log/$san"
  [ $san = asan ] && env="ASAN_OPTIONS=$opts:detect_leaks=1" || env="TSAN_OPTIONS=$opts"
  for input in whip whep_pull; do
    name=q-join-$input-$san
    rm -rf $R5/$name
    bash $T/env_check.sh $R5/$name.env.json > /dev/null || { echo "$name BLOCKED"; continue; }
    ip netns exec $NS python3 $T/aac_join.py --output $R5/$name --runtime $T/rt-s --server-bin build-t/media_server-b-$san --input $input --expect-restarts 1 --lead 12 > $R5/$name.out 2>&1
    echo "== $name exit=$?"; tail -2 $R5/$name.out
    grep -c "Sanitizer" $R5/$name/server.log
  done
  (export $env; SERVER_BIN=build-t/media_server-b-$san run q-lc-$san --input whip --sources 4 --outputs whep:8,flv:4 --workers 4 --server-cpus 0-3 \
    --source-churn 60 --source-churn-input whip --warmup 10 --duration 130 --ready-timeout 180 --cleanup-settle 10 --whep-opus-fmtp "minptime=10;useinbandfec=1" \
    --source-churn-viewers whep:2,flv:2,rtsp:2,rtmp:2 --churn flv:2 --churn-period 15)
  python3 -c "
import json,gzip; r=json.load(gzip.open('$R/q-lc-$san/result.json.gz'))
print('q-lc-$san', r['status'], r.get('boundary'), 'errors', r.get('errors'))
print('churn', r.get('churn',{}).get('summary') if isinstance(r.get('churn'),dict) else r.get('churn'))
for x in r['source_churn']['rounds']: print({k: x.get(k) for k in ('round','publisher_stop','publish_gone_ms','viewers_lingering','ok')})"
  for f in $R5/q-san-$san-log/*; do [ -e "$f" ] || continue; echo "== $f"; grep -v "makecontext/swapcontext" $f | head -30; done
done
echo JOIN_SAN_DONE
