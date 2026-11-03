. /var/tmp/ms-perf-891aec1/jobs-lib/lib.sh
for run in rel-1 rel-2 asan tsan; do
  name=g-life-$run
  variant=${run%%-*}
  ready $name || continue
  mkdir -p $R/$name
  duration=$([ $variant = rel ] && echo 120 || echo 60)
  cd $RT
  ip netns exec $NS env $(san_env $variant $name) MEDIA_SERVER_LOG_LEVEL=debug bash -c "ulimit -n 1048576; ./run.sh python3 bench/perf_run.py --output $R/$name/run \
    --server-bin $(san_bin $variant) --client-dir build --ffmpeg-bin bin/ffmpeg --ffprobe-bin bin/ffprobe \
    --fixture .cache/perf_fixture_720p30.flv --whip-fixture .cache/perf_fixture_whip.mkv \
    --host 198.19.0.10 --client-address 198.19.0.11 --workers 8 --server-cpus 0-7 --client-cpus 8-15,24-31 \
    --min-free-memory-mib 8192 --min-free-disk-mib 4096 --max-server-pss-mib 16384 \
    --inputs whip:8,whep_pull:8 --upstream-sources 8 --upstream-workers 4 --outputs whep_video:32,whep:16,flv:4 \
    --churn whep_video:16 --churn-period 20 --source-churn 20 --source-churn-input whip --source-churn-viewers whep_video:4,whep:2,flv:1 \
    --control-probe-ms 200 --warmup 15 --duration $duration --ready-timeout 120 --ramp-per-second 100 --cleanup-settle 10 --av-probe 20 --perf-stat" > $R/$name.out 2>&1
  echo "== $name exit=$?"; tail -5 $R/$name.out | cut -c1-2000
  python3 $T/jobs-lib/life_summary.py $R/$name/run
  san_report $name
done
echo LIFECYCLE_DONE
