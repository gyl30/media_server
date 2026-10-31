T=/var/tmp/ms-perf-891aec1
NS=msperf-20261013T011210Z-7788320
R=$T/runs4
for input in rtsp_udp rtsp_tcp rtmp; do
  rm -rf $R/layers-$input
  bash $T/env_check.sh $R/layers-$input.env.json > /dev/null || { echo "layers-$input BLOCKED"; continue; }
  ip netns exec $NS python3 $T/rtsp_aac_layers.py --output $R/layers-$input --runtime $T/rt-s --fixture $T/rt-s/fixtures/avsync_720p30.flv \
    --input $input > $R/layers-$input.out 2>&1
  echo "== layers-$input exit=$?"; cat $R/layers-$input.out
done
echo LAYERS_DONE
