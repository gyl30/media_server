T=/var/tmp/ms-perf-891aec1
NS=msperf-20261013T011210Z-7788320
R=$T/runs6
bash $T/cleanup.sh runs6 > /dev/null
mkdir -p $R
cd $T/rt-s
name=v-matrix-opus
rm -rf $R/$name
bash $T/env_check.sh $R/$name.env.json > /dev/null || { echo "$name BLOCKED"; exit 0; }
ip netns exec $NS bash -c "ulimit -n 1048576; ./run.sh python3 regression/protocol_matrix.py --output $R/$name --server-bin build-t/media_server-vc --client-dir build --fixtures fixtures --ffmpeg-bin bin/ffmpeg --ffprobe-bin bin/ffprobe --opus-flv-ffmpeg-bin ffmpeg901/bin/ffmpeg --opus-flv-ffprobe-bin ffmpeg901/bin/ffprobe --only rtsp-tcp-h264-opus rtmp-h264-opus" > $R/$name.log 2>&1
echo "matrix exit=$?"; grep -E "^[a-z0-9-]+: (PASS|FAIL|XFAIL|XPASS)" $R/$name.log
find $R/$name -type f -size +1M -delete
echo MATRIX_OPUS_DONE
