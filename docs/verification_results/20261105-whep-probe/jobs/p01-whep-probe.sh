. /var/tmp/ms-perf-891aec1/jobs-lib-r/s.sh
B=$R/s5-probe/bench-fix
rm -rf $B && cp -r $RT/bench $B && cp $R/s5-probe/tool-fix/whep_dump_av.py $B/whep_dump_av.py
cd $RT
for round in 1 2 3; do
  name=s6-whep-probe-r$round
  ready $name || continue
  mkdir -p $R/$name
  ip netns exec $NS env MEDIA_SERVER_LOG_LEVEL=debug bash -c "ulimit -n 1048576; ./run.sh python3 $B/perf_run.py --output $R/$name/run \
    --server-bin build/media_server --client-dir build --ffmpeg-bin bin/ffmpeg --ffprobe-bin bin/ffprobe \
    --fixture .cache/perf_fixture_720p30.flv --whip-fixture .cache/perf_fixture_whip.mkv \
    --host 198.19.0.10 --client-address 198.19.0.11 --workers 8 --server-cpus 0-7 --client-cpus 8-15,24-31 \
    --min-free-memory-mib 8192 --min-free-disk-mib 4096 --max-server-pss-mib 16384 \
    --inputs whip:8,whep_pull:8 --upstream-sources 8 --upstream-workers 4 --outputs whep_video:32,whep:16,flv:4 \
    --churn whep_video:16 --churn-period 20 --source-churn 20 --source-churn-input whip --source-churn-viewers whep_video:4,whep:2,flv:1 \
    --control-probe-ms 200 --warmup 15 --duration 150 --ready-timeout 120 --ramp-per-second 100 --cleanup-settle 10 --av-probe 20" > $R/$name.out 2>&1
  echo "== $name exit=$?"
  python3 -c "import json; r=json.load(open('$R/$name/run/result.json')); w=r['av_probe']['whep']; print(r['status'], r.get('capacity_boundary'), json.dumps({k: w.get(k) for k in ('ok','audio_frames','audio_pts_gaps','audio_pts_overlaps','audio_pts_gap_max_ms','audio_rtp_lost','recorded_audio_pts_gaps','recorded_audio_pts_gap_max_ms','video_pts_backward','recorded_video_pts_backward')}))"
done
echo P01_DONE
