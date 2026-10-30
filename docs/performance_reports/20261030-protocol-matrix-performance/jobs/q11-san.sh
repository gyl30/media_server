. /var/tmp/ms-perf-891aec1/lib.sh
NS=msperf-20261013T011210Z-7788320
CHROME="minptime=10;useinbandfec=1"
SUB="rtmp-h264-aac rtmp-h264-bframes-aac rtsp-tcp-h264-opus whip-h264-opus whep-pull rtsp-pull gb-udp multi-source-cross-worker fault-source-stop fault-invalid-token-whep fault-replayed-token-whep"
M="--client-dir build --fixtures fixtures --ffmpeg-bin bin/ffmpeg --ffprobe-bin bin/ffprobe --workers 4 --server-cpus 0-3 --client-cpus 8-15 --opus-flv-ffmpeg-bin ffmpeg901/bin/ffmpeg --opus-flv-ffprobe-bin ffmpeg901/bin/ffprobe"
for san in asan tsan; do
  mkdir -p $R/san-$san-log
  opts="log_path=$R/san-$san-log/$san"
  [ $san = asan ] && env="ASAN_OPTIONS=$opts:detect_leaks=1" || env="TSAN_OPTIONS=$opts"
  bash $T/env_check.sh $R/mx-$san.env.json > /dev/null || { echo "mx-$san BLOCKED"; continue; }
  cd $T/rt-q
  ip netns exec $NS bash -c "ulimit -n 1048576; $env ./run.sh python3 regression/protocol_matrix.py --output $R/mx-$san --server-bin build/media_server-$san-head $M --duration 20 --ready-timeout 180 --only $SUB" > $R/mx-$san.log 2>&1
  echo "$san matrix exit=$?"; cat $R/mx-$san.log
  cd $T
  SERVER_BIN=build/media_server-$san-head SERVER_ENV="$env" run lc-$san --input whip --sources 4 --outputs whep:8,flv:8 --workers 4 --server-cpus 0-3 \
    --source-churn 60 --source-churn-input whip --warmup 10 --duration 130 --ready-timeout 180 --cleanup-settle 10 --whep-opus-fmtp "$CHROME" \
    --source-churn-viewers whep:2,flv:2,gb_udp:1,gb_tcp_passive:1,hls:1,gb_tcp_active:1,rtsp:2,rtmp:2
  python3 -c "
import json,gzip; r=json.load(gzip.open('$R/lc-$san/result.json.gz'))
for x in r['source_churn']['rounds']: print({k: x.get(k) for k in ('round','publisher_stop','publish_gone_ms','viewers_lingering','ok')})"
  for f in $R/san-$san-log/*; do [ -e "$f" ] || continue; echo "== $f"; grep -v "makecontext/swapcontext" $f | head -30; done
done
echo SAN_DONE
