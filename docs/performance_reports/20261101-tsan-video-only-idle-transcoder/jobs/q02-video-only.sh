T=/var/tmp/ms-perf-891aec1
NS=msperf-20261013T011210Z-7788320
R=$T/runs5
mkdir -p $R
for rep in 1 2; do
  for sources in 64 256; do
    name=q-video-$sources-r$rep
    rm -rf $R/$name
    bash $T/env_check.sh $R/$name.env.json > /dev/null || { echo "$name BLOCKED"; continue; }
    ip netns exec $NS bash -c "ulimit -n 1048576; python3 $T/viewer_cost.py --output $R/$name --runtime $T/rt-s --server-bin build-t/media_server-a --sources $sources --steps idle,video,passthrough,video_flv,idle --trace $T/opus_aac.bt" > $R/$name.out 2>&1
    echo "== $name exit=$?"; cat $R/$name.out
  done
done
echo VIDEO_DONE
