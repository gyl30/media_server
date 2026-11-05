#!/bin/bash
# Full protocol matrix with the RC package's media_server and the updated WHEP verdict (runtime rt-a = rt-r plus the changed tools).
# Self-contained: it does not source the older job libraries, whose start-up cleanup deletes large media files under runs*.
T=/var/tmp/ms-perf-891aec1
NS=msperf-20261013T011210Z-7788320
SERVER=/var/tmp/ms-rc-2adfaea/media_server-2adfaea28ad2/bin/media_server
OUT=$T/runs12/a-matrix-rc
[ ! -e $OUT ] || { echo "$OUT exists"; exit 1; }
cd $T/rt-a
ip netns exec $NS bash -c "ulimit -n 1048576; ./run.sh python3 regression/protocol_matrix.py --output $OUT --server-bin $SERVER \
  --client-dir build --fixtures fixtures --ffmpeg-bin bin/ffmpeg --ffprobe-bin bin/ffprobe --workers 4 --server-cpus 0-3 --client-cpus 8-15 \
  --opus-flv-ffmpeg-bin ffmpeg901/bin/ffmpeg --opus-flv-ffprobe-bin ffmpeg901/bin/ffprobe" > $OUT.log 2>&1
echo "matrix exit=$?"
python3 - $OUT <<'PY'
import json, sys
summary = json.load(open(sys.argv[1] + "/summary.json"))
counts = {}
for name, item in summary.items():
    counts[item.get("status")] = counts.get(item.get("status"), 0) + 1
    whep = item.get("av_probe", {}).get("whep") or {}
    print(name, item.get("status"), item.get("known_issue", ""), json.dumps({key: whep.get(key) for key in
          ("ok", "verdict", "audio_rtp_packets", "audio_rtp_lost", "audio_pts_gaps", "audio_pts_overlaps")}) if whep else "")
print("counts", counts)
PY
