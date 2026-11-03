T=/var/tmp/ms-perf-891aec1
NS=msperf-20261013T011210Z-7788320
R=$T/runs6
bash $T/cleanup.sh runs6 > /dev/null
mkdir -p $R
for san in asan tsan; do
  name=v-san-$san
  rm -rf $R/$name
  bash $T/env_check.sh $R/$name.env.json > /dev/null || { echo "$name BLOCKED"; continue; }
  ip netns exec $NS bash -c "ulimit -n 1048576; python3 $T/viewer_cost.py --output $R/$name --runtime $T/rt-s --server-bin build-t/media_server-vc-$san --sources 8 --steps video,passthrough,video_flv,flv:4,video,video,video_stop --settle 10 --window 20" > $R/$name.out 2>&1
  echo "== $name exit=$?"; cat $R/$name.out
  echo "sanitizer lines: $(grep 'Sanitizer\|runtime error' $R/$name/server.log | grep -vc 'makecontext/swapcontext')"
  grep -A12 "WARNING: ThreadSanitizer\|ERROR: AddressSanitizer\|ERROR: LeakSanitizer\|runtime error" $R/$name/server.log | head -60
done
echo SAN_DONE
