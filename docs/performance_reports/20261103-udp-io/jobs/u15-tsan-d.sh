. /var/tmp/ms-perf-891aec1/jobs-lib-u/udp.sh
for b in san/media_server-gso-tsan san/media_server-tsan; do
  name=u15-tsan-d-$(basename $b)
  ready $name || continue
  mkdir -p $R/$name
  eval "in=\$MIX_D_IN out=\$MIX_D_OUT"
  cd $T/rt-g
  ip netns exec $NS env $(san_env tsan $name) bash -c "ulimit -n 1048576; ./run.sh python3 bench/perf_run.py --output $R/$name/run \
    --server-bin $b --client-dir build --ffmpeg-bin bin/ffmpeg --ffprobe-bin bin/ffprobe \
    --fixture .cache/perf_fixture_720p30.flv --whip-fixture .cache/perf_fixture_whip.mkv \
    --host 198.19.0.10 --client-address 198.19.0.11 --workers 8 --server-cpus 0-7 --client-cpus 8-15,24-31 \
    --min-free-memory-mib 8192 --min-free-disk-mib 4096 --max-server-pss-mib 16384 --inputs $(scale 0.25 $in) --outputs $(scale 0.25 $out) \
    --upstream-workers 16 --upstream-sources 64 --upstream-bin $T/rt-e1/upstream.sh --control-probe-ms 200 --warmup 15 --duration 60 --ready-timeout 300 \
    --ramp-per-second 100 --group-size 256 --cleanup-settle 5 --viewer-interleave-seed 11 --pull-interleave-seed 11 --source-churn 20 --churn whep:16 --churn-period 15" > $R/$name.out 2>&1
  echo "== $name exit=$?"; tail -2 $R/$name.out | cut -c1-600
  python3 $T/jobs-lib/life_summary.py $R/$name/run | cut -c1-800
  grep -c "srtp protect failed" $R/$name/run/server.log
  san_report $name
done
echo U15_DONE
