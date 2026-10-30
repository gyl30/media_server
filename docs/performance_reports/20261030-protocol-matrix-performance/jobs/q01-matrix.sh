T=/var/tmp/ms-perf-891aec1
NS=msperf-20261013T011210Z-7788320
mkdir -p $T/runs2
bash $T/env_check.sh $T/runs2/mx-rel.env.json || { echo BLOCKED; exit 3; }
cd $T/rt-q
ip netns exec $NS bash -c "ulimit -n 1048576; ./run.sh python3 regression/protocol_matrix.py --output $T/runs2/mx-rel --server-bin build/media_server --client-dir build --fixtures fixtures --ffmpeg-bin bin/ffmpeg --ffprobe-bin bin/ffprobe --workers 4 --server-cpus 0-3 --client-cpus 8-15 --opus-flv-ffmpeg-bin ffmpeg901/bin/ffmpeg --opus-flv-ffprobe-bin ffmpeg901/bin/ffprobe" > $T/runs2/mx-rel.log 2>&1
echo "matrix exit=$?"
cat $T/runs2/mx-rel.log
