T=/var/tmp/ms-perf-891aec1
NS=msperf-20261013T011210Z-7788320
R=$T/runs4
cd $T/rt-s
for san in asan tsan; do
  if [ $san = asan ]; then key=ASAN_OPTIONS; extra=":detect_leaks=1"; else key=TSAN_OPTIONS; extra=""; fi
  M="--client-dir build-p --fixtures fixtures --ffmpeg-bin bin/ffmpeg --ffprobe-bin bin/ffprobe --workers 4 --server-cpus 0-3 --client-cpus 8-15 --ready-timeout 120 --opus-flv-ffmpeg-bin ffmpeg901/bin/ffmpeg --opus-flv-ffprobe-bin ffmpeg901/bin/ffprobe"
  rm -rf $R/p-san-$san; mkdir -p $R/p-san-$san
  bash $T/env_check.sh $R/p-san-$san.env.json > /dev/null || { echo "p-san-$san BLOCKED"; continue; }
  ip netns exec $NS bash -c "ulimit -n 1048576; $key=log_path=$R/p-san-$san/sanitizer$extra ./run.sh python3 regression/protocol_matrix.py --output $R/p-san-$san/matrix \
    --server-bin san-$san/media_server $M --only whip-h264-opus whep-pull whip-chrome-offer whep-pull-chrome-offer rtsp-tcp-h264-opus fault-source-stop" > $R/p-san-$san/matrix.log 2>&1
  echo "matrix-$san exit=$?"; cat $R/p-san-$san/matrix.log
  for d in $R/p-san-$san/matrix/*/; do find $d -type f \( -name "*.rtp" -o -name "*.dump" -o -name "*.mkv" -o -name "*.ts" \) -size +100k -delete; done
  echo "== $san sanitizer files: $(ls $R/p-san-$san | grep -c '^sanitizer')"; cat $R/p-san-$san/sanitizer* 2>/dev/null | head -60
done
echo SAN_DONE
