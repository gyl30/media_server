. /var/tmp/ms-perf-891aec1/lib3.sh
NS=msperf-20261013T011210Z-7788320
CHROME="minptime=10;useinbandfec=1"
SUB="rtmp-h264-aac whip-h264-opus whep-pull rtsp-pull gb-udp gb-tcp-active multi-source-cross-worker fault-source-stop fault-invalid-token-whep"
M="--client-dir build --fixtures fixtures --ffmpeg-bin bin/ffmpeg --ffprobe-bin bin/ffprobe --workers 4 --server-cpus 0-3 --client-cpus 8-15"
for san in asan tsan; do
  mkdir -p $R/san-$san-log
  opts="log_path=$R/san-$san-log/$san"
  [ $san = asan ] && env="ASAN_OPTIONS=$opts:detect_leaks=1" || env="TSAN_OPTIONS=$opts"
  bash $T/env_check.sh $R/mx-$san.env.json > /dev/null || { echo "mx-$san BLOCKED"; continue; }
  cd $T/rt-r
  ip netns exec $NS bash -c "ulimit -n 1048576; $env ./run.sh python3 regression/protocol_matrix.py --output $R/mx-$san --server-bin build/media_server-$san-b $M --duration 20 --ready-timeout 180 --only $SUB" > $R/mx-$san.log 2>&1
  echo "$san matrix exit=$?"; cat $R/mx-$san.log
  cd $T
  SERVER_BIN=build/media_server-$san-b SERVER_ENV="$env" run lc-$san --input whip --sources 4 --outputs whep:8,flv:4 --workers 4 --server-cpus 0-3 \
    --source-churn 60 --source-churn-input whip --warmup 10 --duration 130 --ready-timeout 180 --cleanup-settle 10 --whep-opus-fmtp "$CHROME" \
    --source-churn-viewers whep:2,flv:2,gb_udp:1,gb_tcp_passive:1,hls:1,gb_tcp_active:1,rtsp:2,rtmp:2 --churn flv:2 --churn-period 15
  python3 $T/st.py $R/lc-$san
  python3 -c "
import json,gzip; r=json.load(gzip.open('$R/lc-$san/result.json.gz'))
print('churn', [x.get('ok') for x in r.get('churn',{}).get('rounds',[])] if isinstance(r.get('churn'),dict) else r.get('churn'))
for x in r['source_churn']['rounds']: print({k: x.get(k) for k in ('round','publisher_stop','publish_gone_ms','viewers_lingering','ok')})"
  for input in whip whep_pull; do
    bash $T/env_check.sh $R/join-$input-$san.env.json > /dev/null || { echo "join-$input-$san BLOCKED"; continue; }
    ip netns exec $NS python3 $T/aac_join.py --output $R/join-$input-$san --runtime $T/rt-r --server-bin build/media_server-$san-b --input $input --expect-restarts 1 --lead 12 > $R/join-$input-$san.out 2>&1
    tail -2 $R/join-$input-$san.out
    grep -c "Sanitizer" $R/join-$input-$san/server.log
  done
  for f in $R/san-$san-log/*; do [ -e "$f" ] || continue; echo "== $f"; grep -v "makecontext/swapcontext" $f | head -30; done
done
echo SAN_DONE
