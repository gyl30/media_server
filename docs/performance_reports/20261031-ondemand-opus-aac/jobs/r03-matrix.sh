T=/var/tmp/ms-perf-891aec1
NS=msperf-20261013T011210Z-7788320
M="--client-dir build --fixtures fixtures --ffmpeg-bin bin/ffmpeg --ffprobe-bin bin/ffprobe --workers 4 --server-cpus 0-3 --client-cpus 8-15 --opus-flv-ffmpeg-bin ffmpeg901/bin/ffmpeg --opus-flv-ffprobe-bin ffmpeg901/bin/ffprobe"
for variant in a b; do
  bin=$([ $variant = a ] && echo build/media_server || echo build-b/media_server)
  bash $T/env_check.sh $T/runs3/mx-$variant.env.json > /dev/null || { echo "mx-$variant BLOCKED"; continue; }
  cd $T/rt-r
  ip netns exec $NS bash -c "ulimit -n 1048576; ./run.sh python3 regression/protocol_matrix.py --output $T/runs3/mx-$variant --server-bin $bin $M" > $T/runs3/mx-$variant.log 2>&1
  echo "mx-$variant exit=$?"; cat $T/runs3/mx-$variant.log
done
echo MATRIX_DONE
