. /var/tmp/ms-perf-891aec1/jobs-lib-l/l.sh
M="--client-dir build --fixtures fixtures --ffmpeg-bin bin/ffmpeg --ffprobe-bin bin/ffprobe --workers 4 --server-cpus 0-3 --client-cpus 8-15 --opus-flv-ffmpeg-bin ffmpeg901/bin/ffmpeg --opus-flv-ffprobe-bin ffmpeg901/bin/ffprobe"
bin_of() { case $1 in rel) echo build/media_server;; *) echo san/media_server-$1;; esac; }
summary() {
  python3 - $1 <<'PY'
import json, sys
s = json.load(open(sys.argv[1] + "/summary.json"))
counts = {}
for name, item in s.items():
    counts[item.get("status")] = counts.get(item.get("status"), 0) + 1
    print(name, item.get("status"), item.get("known_issue", ""), item.get("reason", ""))
print("counts", counts)
PY
}
strip() { for d in $1/*/; do find $d -type f \( -name "*.rtp" -o -name "*.dump" -o -name "*.mkv" -o -name "*.ts" -o -name "*.flv" -o -name "*.es" \) -size +100k -delete; done; }
cd $RT

name=l2-matrix-rel
if ready $name; then
  ip netns exec $NS bash -c "ulimit -n 1048576; ./run.sh python3 regression/protocol_matrix.py --output $R/$name --server-bin $(bin_of rel) $M" > $R/$name.log 2>&1
  echo "$name exit=$?"; summary $R/$name; strip $R/$name
fi
SUBSET_ASAN="rtmp-h264-aac rtmp-h265-aac rtmp-h264-pcma rtsp-tcp-h264-aac rtsp-udp-h264-aac rtsp-tcp-h264-opus whip-h264-opus rtmp-pull rtsp-pull whep-pull whip-chrome-offer whep-pull-chrome-offer whip-video-only-whep whep-pull-video-only-whep gb-udp gb-tcp-active gb-tcp-passive multi-source-cross-worker slow-readers fault-source-stop fault-invalid-token-whep fault-replayed-token-whep"
SUBSET_TSAN="rtmp-h264-aac rtsp-udp-h264-aac whip-h264-opus rtsp-pull whep-pull whep-pull-chrome-offer whep-pull-video-only-whep gb-udp gb-tcp-passive multi-source-cross-worker slow-readers fault-source-stop"
for san in asan tsan; do
  name=l2-matrix-$san
  ready $name || continue
  mkdir -p $R/$name
  subset=$([ $san = asan ] && echo "$SUBSET_ASAN" || echo "$SUBSET_TSAN")
  ip netns exec $NS env $(san_env $san $name) bash -c "ulimit -n 1048576; ./run.sh python3 regression/protocol_matrix.py --output $R/$name/matrix \
    --server-bin $(bin_of $san) $M --ready-timeout 120 --only $subset" > $R/$name/matrix.log 2>&1
  echo "$name exit=$?"; summary $R/$name/matrix; strip $R/$name/matrix
  san_report $name
done

for run in rel asan tsan; do
  name=l2-boundary-$run
  ready $name || continue
  mkdir -p $R/$name
  ip netns exec $NS env $(san_env $run $name) PYTHONPATH=$T/pwlib python3 $T/jobs-lib/whep_boundary.py --output $R/$name/run --runtime $RT \
    --server-bin $(bin_of $run) --chrome-bin $CHROME --seconds 60 > $R/$name.out 2>&1
  echo "== $name exit=$?"; cut -c1-3000 $R/$name.out
  san_report $name
done

for run in rel asan tsan; do
  duration=$([ $run = rel ] && echo 120 || echo 60)
  name=l2-life-whep-$run
  if ready $name; then
    mkdir -p $R/$name
    ip netns exec $NS env $(san_env $run $name) MEDIA_SERVER_LOG_LEVEL=debug bash -c "ulimit -n 1048576; ./run.sh python3 bench/perf_run.py --output $R/$name/run \
      --server-bin $(bin_of $run) --client-dir build --ffmpeg-bin bin/ffmpeg --ffprobe-bin bin/ffprobe \
      --fixture .cache/perf_fixture_720p30.flv --whip-fixture .cache/perf_fixture_whip.mkv \
      --host 198.19.0.10 --client-address 198.19.0.11 --workers 8 --server-cpus 0-7 --client-cpus 8-15,24-31 \
      --min-free-memory-mib 8192 --min-free-disk-mib 4096 --max-server-pss-mib 16384 \
      --inputs whip:8,whep_pull:8 --upstream-sources 8 --upstream-workers 4 --outputs whep_video:32,whep:16,flv:4 \
      --churn whep_video:16 --churn-period 20 --source-churn 20 --source-churn-input whip --source-churn-viewers whep_video:4,whep:2,flv:1 \
      --control-probe-ms 200 --warmup 15 --duration $duration --ready-timeout 120 --ramp-per-second 100 --cleanup-settle 10 --av-probe 20 --perf-stat" > $R/$name.out 2>&1
    echo "== $name exit=$?"; tail -3 $R/$name.out | cut -c1-1500
    python3 $T/jobs-lib/life_summary.py $R/$name/run
    san_report $name
  fi
  name=l2-life-gb-$run
  if ready $name; then
    mkdir -p $R/$name
    ip netns exec $NS env $(san_env $run $name) MEDIA_SERVER_LOG_LEVEL=debug bash -c "ulimit -n 1048576; ./run.sh python3 bench/perf_run.py --output $R/$name/run \
      --server-bin $(bin_of $run) --client-dir build --ffmpeg-bin bin/ffmpeg --ffprobe-bin bin/ffprobe \
      --fixture .cache/perf_fixture_720p30.flv --whip-fixture .cache/perf_fixture_whip.mkv \
      --host 198.19.0.10 --client-address 198.19.0.11 --workers 8 --server-cpus 0-7 --client-cpus 8-15,24-31 \
      --min-free-memory-mib 8192 --min-free-disk-mib 4096 --max-server-pss-mib 16384 \
      --inputs rtmp:8 --outputs gb_udp:256,whep:16,flv:4 \
      --churn gb_udp:64 --churn-period 20 --source-churn 20 --source-churn-input rtmp --source-churn-viewers gb_udp:8,whep:2,flv:1 \
      --control-probe-ms 200 --warmup 15 --duration $duration --ready-timeout 120 --ramp-per-second 100 --cleanup-settle 10 --perf-stat" > $R/$name.out 2>&1
    echo "== $name exit=$?"; tail -3 $R/$name.out | cut -c1-1500
    python3 $T/jobs-lib/life_summary.py $R/$name/run
    san_report $name
  fi
done
echo L02_DONE
