T=/var/tmp/ms-perf-891aec1
NS=msperf-20261013T011210Z-7788320
M="--client-dir build --fixtures fixtures --ffmpeg-bin bin/ffmpeg --ffprobe-bin bin/ffprobe --workers 4 --server-cpus 0-3 --client-cpus 8-15 --opus-flv-ffmpeg-bin ffmpeg901/bin/ffmpeg --opus-flv-ffprobe-bin ffmpeg901/bin/ffprobe"
bash $T/env_check.sh $T/runs4/mx-full.env.json > /dev/null || { echo "mx-full BLOCKED"; exit; }
cd $T/rt-s
ip netns exec $NS bash -c "ulimit -n 1048576; ./run.sh python3 regression/protocol_matrix.py --output $T/runs4/mx-full --server-bin build/media_server $M" > $T/runs4/mx-full.log 2>&1
echo "mx-full exit=$?"; cat $T/runs4/mx-full.log
python3 - <<'PY'
import json
from pathlib import Path
for path in sorted(Path("/var/tmp/ms-perf-891aec1/runs4/mx-full").glob("*/result.json")):
    probes = json.loads(path.read_text()).get("av_probe") or {}
    for name, probe in probes.items():
        if name.startswith("gb_"):
            print(path.parent.name, name, probe.get("ok"), probe.get("audio_codec"), probe.get("av_markers"), probe.get("audio_pts_gaps"),
                  probe.get("audio_pts_slip_ms_max"), (probe.get("reference") or {}).get("ok"),
                  sorted(key for key, value in (probe.get("checks") or {}).items() if not value))
PY
echo MATRIX_DONE
