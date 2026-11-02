T=/var/tmp/ms-perf-891aec1
NS=msperf-20261013T011210Z-7788320
R=$T/runs5
mkdir -p $R
for rep in 1 2; do
  for sources in 64 256; do
    for variant in a c; do
      name=q-vc-$sources-$variant-r$rep
      rm -rf $R/$name
      bash $T/env_check.sh $R/$name.env.json > /dev/null || { echo "$name BLOCKED"; continue; }
      ip netns exec $NS bash -c "ulimit -n 1048576; python3 $T/viewer_cost.py --output $R/$name --runtime $T/rt-s --server-bin build-t/media_server-$variant --sources $sources --steps idle,video,passthrough,video_flv,video_stop --trace $T/opus_aac.bt" > $R/$name.out 2>&1
      echo "== $name exit=$?"; cat $R/$name.out
    done
  done
done
for san in asan tsan; do
  name=q-vc-san-$san
  rm -rf $R/$name
  bash $T/env_check.sh $R/$name.env.json > /dev/null || { echo "$name BLOCKED"; continue; }
  ip netns exec $NS bash -c "ulimit -n 1048576; python3 $T/viewer_cost.py --output $R/$name --runtime $T/rt-s --server-bin build-t/media_server-c-$san --sources 8 --steps video,passthrough,video_flv,flv:4,video_stop --settle 10 --window 20" > $R/$name.out 2>&1
  echo "== $name exit=$?"; cat $R/$name.out
  echo "sanitizer lines: $(grep 'Sanitizer\|runtime error' $R/$name/server.log | grep -vc 'makecontext/swapcontext')"
  grep -A8 "WARNING: ThreadSanitizer\|ERROR: AddressSanitizer\|ERROR: LeakSanitizer\|runtime error" $R/$name/server.log | head -40
done
cd $T/rt-s
name=q-vc-matrix
rm -rf $R/$name
bash $T/env_check.sh $R/$name.env.json > /dev/null && ip netns exec $NS bash -c "ulimit -n 1048576; ./run.sh python3 regression/protocol_matrix.py --output $R/$name --server-bin build-t/media_server-c --client-dir build --fixtures fixtures --ffmpeg-bin bin/ffmpeg --ffprobe-bin bin/ffprobe --only whip-h264-opus whep-pull whip-chrome-offer whep-pull-chrome-offer rtmp-h264-aac fault-source-stop" > $R/$name.log 2>&1
echo "matrix exit=$?"; grep -E "^[a-z0-9-]+ (PASS|FAIL|XFAIL|XPASS)" $R/$name.log
find $R/$name -type f -size +1M -delete
echo CANDIDATE_DONE
