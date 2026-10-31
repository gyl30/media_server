T=/var/tmp/ms-perf-891aec1
NS=msperf-20261013T011210Z-7788320
mkdir -p $T/runs3
bash $T/env_check.sh $T/runs3/mx-a-smoke.env.json > /dev/null || { echo BLOCKED; exit 3; }
cd $T/rt-r
ip netns exec $NS bash -c "ulimit -n 1048576; ./run.sh python3 regression/protocol_matrix.py --output $T/runs3/mx-a-smoke --server-bin build/media_server --client-dir build --fixtures fixtures --ffmpeg-bin bin/ffmpeg --ffprobe-bin bin/ffprobe --workers 4 --server-cpus 0-3 --client-cpus 8-15 --only rtmp-h265-aac rtsp-tcp-h265-aac rtmp-h264-aac rtmp-h264-pcma gb-udp fault-source-stop" > $T/runs3/mx-a-smoke.log 2>&1
echo "matrix exit=$?"
cat $T/runs3/mx-a-smoke.log
python3 - <<PY
import json
s=json.load(open("$T/runs3/mx-a-smoke/summary.json"))
for name,v in s.items():
    print(name, v["status"], {k:{x:p.get(x) for x in ("ok","video_frames","audio_frames","av_markers","av_offset_ms_p50","decode_warnings","video_pts_backward","audio_pts_gaps")} for k,p in v.get("av_probe",{}).items() if k.startswith(("gb","whep"))})
    print("  groups", {g:(x["ready"],x["requested"],x["failure_category"]) for g,x in v["groups"].items()}, "cleanup", v["cleanup"])
PY
echo SMOKE_DONE
