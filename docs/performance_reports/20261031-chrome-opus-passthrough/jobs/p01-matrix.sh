T=/var/tmp/ms-perf-891aec1
NS=msperf-20261013T011210Z-7788320
R=$T/runs4
M="--client-dir build-p --fixtures fixtures --ffmpeg-bin bin/ffmpeg --ffprobe-bin bin/ffprobe --workers 4 --server-cpus 0-3 --client-cpus 8-15 --opus-flv-ffmpeg-bin ffmpeg901/bin/ffmpeg --opus-flv-ffprobe-bin ffmpeg901/bin/ffprobe"
cd $T/rt-s
build-p/media_server --version
bash $T/env_check.sh $R/p-matrix.env.json > /dev/null || { echo "p-matrix BLOCKED"; exit; }
ip netns exec $NS bash -c "ulimit -n 1048576; ./run.sh python3 regression/protocol_matrix.py --output $R/p-matrix --server-bin build-p/media_server $M" > $R/p-matrix.log 2>&1
echo "p-matrix exit=$?"; cat $R/p-matrix.log
python3 - <<'PY'
import json
s = json.load(open("/var/tmp/ms-perf-891aec1/runs4/p-matrix/summary.json"))
for name, item in s.items():
    print(name, item.get("status"), item.get("known_issue", ""))
PY
for name in whip-chrome-offer whep-pull-chrome-offer whip-h264-opus whep-pull; do
  echo "$name egress=$(grep -c 'output opus' $R/p-matrix/$name/server.log) opus_aac=$(grep -c 'input opus' $R/p-matrix/$name/server.log)"
done
for d in $R/p-matrix/*/; do find $d -type f \( -name "*.rtp" -o -name "*.dump" -o -name "*.mkv" -o -name "*.ts" \) -size +100k -delete; done
echo MATRIX_DONE
