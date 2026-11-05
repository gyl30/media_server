. /var/tmp/ms-perf-891aec1/jobs-lib-r/s.sh
cd $RT
for round in 1 2 3; do
  name=s3-whep-gap-r$round
  ready $name || continue
  mkdir -p $R/$name
  ip netns exec $NS env MEDIA_SERVER_LOG_LEVEL=debug bash -c "ulimit -n 1048576; ./run.sh python3 bench/perf_run.py --output $R/$name/run \
    --server-bin build/media_server --client-dir build --ffmpeg-bin bin/ffmpeg --ffprobe-bin bin/ffprobe \
    --fixture .cache/perf_fixture_720p30.flv --whip-fixture .cache/perf_fixture_whip.mkv \
    --host 198.19.0.10 --client-address 198.19.0.11 --workers 8 --server-cpus 0-7 --client-cpus 8-15,24-31 \
    --min-free-memory-mib 8192 --min-free-disk-mib 4096 --max-server-pss-mib 16384 \
    --inputs whip:8,whep_pull:8 --upstream-sources 8 --upstream-workers 4 --outputs whep_video:32,whep:16,flv:4 \
    --churn whep_video:16 --churn-period 20 --source-churn 20 --source-churn-input whip --source-churn-viewers whep_video:4,whep:2,flv:1 \
    --control-probe-ms 200 --warmup 15 --duration 150 --ready-timeout 120 --ramp-per-second 100 --cleanup-settle 10 --av-probe 20" > $R/$name.out 2>&1
  echo "== $name exit=$?"
  python3 -c "import json; r=json.load(open('$R/$name/run/result.json')); print(r['status'], json.dumps({k: r['av_probe'][k].get('audio_pts_gaps') for k in r['av_probe']}), r['av_probe']['whep'].get('audio_pts_gap_max_ms'))"
  python3 $T/dumpseq.py $R/$name/run/whep-0.dump
  ls -la $R/$name/run/whep-0.dump
done
echo S03_DONE
