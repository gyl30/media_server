. /var/tmp/ms-perf-891aec1/jobs-lib-r/s.sh
cd $RT
life() {
  name=$1; shift
  ready $name || return 0
  mkdir -p $R/$name
  ip netns exec $NS env MEDIA_SERVER_LOG_LEVEL=debug bash -c "ulimit -n 1048576; ./run.sh python3 bench/perf_run.py --output $R/$name/run \
    --server-bin build/media_server --client-dir build --ffmpeg-bin bin/ffmpeg --ffprobe-bin bin/ffprobe \
    --fixture .cache/perf_fixture_720p30.flv --whip-fixture .cache/perf_fixture_whip.mkv \
    --host 198.19.0.10 --client-address 198.19.0.11 --workers 8 --server-cpus 0-7 --client-cpus 8-15,24-31 \
    --min-free-memory-mib 8192 --min-free-disk-mib 4096 --max-server-pss-mib 16384 \
    --churn-period 20 --source-churn 20 --control-probe-ms 200 --warmup 15 --duration 600 --ready-timeout 120 \
    --ramp-per-second 100 --cleanup-settle 10 --perf-stat $*" > $R/$name.out 2>&1
  echo "== $name exit=$?"; tail -3 $R/$name.out | cut -c1-1500
  python3 $T/jobs-lib/life_summary.py $R/$name/run
  find $R/$name -type f \( -name "*.dump" -o -name "*.mkv" -o -name "*.rtp" -o -name "*.ts" -o -name "*.flv" -o -name "*.es" \) -size +100k -delete
}
life s0-smoke-rtsp --inputs rtmp:4,rtsp_tcp:4,rtsp_udp:4 --outputs rtsp:64,whep:8,flv:4 \
  --churn rtsp:32 --source-churn-input rtmp --source-churn-viewers rtsp:4,whep:2,flv:1 --duration 30
life s0-smoke-rtmp --inputs rtmp:8 --outputs rtmp:64,flv:32,whep:8 \
  --churn rtmp:64 --source-churn-input rtmp --source-churn-viewers rtmp:4,flv:2,whep:1 --duration 30
echo S00_DONE
