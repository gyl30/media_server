T=/var/tmp/ms-perf-891aec1
NS=msperf-20261013T011210Z-7788320
R=$T/runs4
mkdir -p $R
M="--client-dir build --fixtures fixtures --ffmpeg-bin bin/ffmpeg --ffprobe-bin bin/ffprobe --workers 4 --server-cpus 0-3 --client-cpus 8-15"
name=mx-gb-codecs
bash $T/env_check.sh $R/$name.env.json > /dev/null || { echo "$name BLOCKED"; exit; }
cd $T/rt-s
ip netns exec $NS bash -c "ulimit -n 1048576; ./run.sh python3 regression/protocol_matrix.py --output $R/$name --server-bin build/media_server $M \
  --only rtmp-h264-pcmu rtmp-h264-aac rtmp-h264-pcma rtmp-h265-aac" > $R/$name.log 2>&1
echo "$name exit=$?"; cat $R/$name.log
python3 - <<PY
import json
s = json.load(open("$R/$name/summary.json"))
for scenario, value in s.items():
    for protocol, probe in value.get("av_probe", {}).items():
        if protocol.startswith("gb"):
            print(scenario, protocol, probe.get("ok"), probe.get("audio_codec"), probe.get("video_frames"), probe.get("audio_frames"), probe.get("audio_samples"),
                  probe.get("av_markers"), probe.get("av_offset_ms_p50"), probe.get("audio_pts_gaps"), probe.get("checks"), (probe.get("reference") or {}).get("status"),
                  (probe.get("reference") or {}).get("matched"), (probe.get("reference") or {}).get("jump_count"))
PY
echo GB_CODECS_DONE
