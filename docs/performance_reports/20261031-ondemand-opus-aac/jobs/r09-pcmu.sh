T=/var/tmp/ms-perf-891aec1
NS=msperf-20261013T011210Z-7788320
M="--client-dir build --fixtures fixtures --ffmpeg-bin bin/ffmpeg --ffprobe-bin bin/ffprobe --workers 4 --server-cpus 0-3 --client-cpus 8-15"
for variant in a b; do
  bin=$([ $variant = a ] && echo build/media_server || echo build-b/media_server)
  bash $T/env_check.sh $T/runs3/mx-$variant-pcmu.env.json > /dev/null || { echo "mx-$variant-pcmu BLOCKED"; continue; }
  cd $T/rt-r
  ip netns exec $NS bash -c "ulimit -n 1048576; ./run.sh python3 regression/protocol_matrix.py --output $T/runs3/mx-$variant-pcmu --server-bin $bin $M --only rtmp-h264-pcmu" > $T/runs3/mx-$variant-pcmu.log 2>&1
  echo "mx-$variant-pcmu exit=$?"; cat $T/runs3/mx-$variant-pcmu.log
  python3 -c "
import json
s=json.load(open('$T/runs3/mx-$variant-pcmu/summary.json'))['rtmp-h264-pcmu']
print({k:(p.get('ok'),p.get('video_frames'),p.get('audio_frames'),p.get('audio_unverified')) for k,p in s['av_probe'].items()})"
done
echo PCMU_DONE
