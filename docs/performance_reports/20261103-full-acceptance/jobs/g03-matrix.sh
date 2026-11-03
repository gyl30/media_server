. /var/tmp/ms-perf-891aec1/jobs-lib/lib.sh
M="--client-dir build --fixtures fixtures --ffmpeg-bin bin/ffmpeg --ffprobe-bin bin/ffprobe --workers 4 --server-cpus 0-3 --client-cpus 8-15 --opus-flv-ffmpeg-bin ffmpeg901/bin/ffmpeg --opus-flv-ffprobe-bin ffmpeg901/bin/ffprobe"
cd $RT
build/media_server --version
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
name=g-matrix-rel
if ready $name; then
  ip netns exec $NS bash -c "ulimit -n 1048576; ./run.sh python3 regression/protocol_matrix.py --output $R/$name --server-bin build/media_server $M" > $R/$name.log 2>&1
  echo "$name exit=$?"; tail -40 $R/$name.log; summary $R/$name; strip $R/$name
fi
SUBSET_ASAN="rtmp-h264-aac rtmp-h265-aac rtmp-h264-pcma rtsp-tcp-h264-aac rtsp-udp-h264-aac rtsp-tcp-h264-opus whip-h264-opus rtmp-pull rtsp-pull whep-pull whip-chrome-offer whep-pull-chrome-offer whip-video-only-whep whep-pull-video-only-whep gb-udp gb-tcp-active gb-tcp-passive multi-source-cross-worker slow-readers fault-source-stop fault-invalid-token-whep fault-replayed-token-whep"
SUBSET_TSAN="rtmp-h264-aac rtsp-udp-h264-aac whip-h264-opus rtsp-pull whep-pull whep-pull-chrome-offer whep-pull-video-only-whep gb-udp gb-tcp-passive multi-source-cross-worker slow-readers fault-source-stop"
for san in asan tsan; do
  name=g-matrix-$san
  ready $name || continue
  mkdir -p $R/$name
  subset=$([ $san = asan ] && echo "$SUBSET_ASAN" || echo "$SUBSET_TSAN")
  ip netns exec $NS env $(san_env $san $name) bash -c "ulimit -n 1048576; ./run.sh python3 regression/protocol_matrix.py --output $R/$name/matrix \
    --server-bin san/media_server-$san $M --ready-timeout 120 --only $subset" > $R/$name/matrix.log 2>&1
  echo "$name exit=$?"; tail -30 $R/$name/matrix.log; summary $R/$name/matrix; strip $R/$name/matrix
  san_report $name
done
echo MATRIX_DONE
