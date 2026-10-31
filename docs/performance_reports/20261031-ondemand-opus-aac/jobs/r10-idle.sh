T=/var/tmp/ms-perf-891aec1
NS=msperf-20261013T011210Z-7788320
R=$T/runs3
for rep in 1 2; do
  for variant in a b; do
    name=idle-$variant-$rep
    bin=$([ $variant = a ] && echo build/media_server || echo build-b/media_server)
    bash $T/env_check.sh $R/$name.env.json > /dev/null || { echo "$name BLOCKED"; continue; }
    ip netns exec $NS bash -c "ulimit -n 1048576; python3 $T/idle_sources.py --output $R/$name --runtime $T/rt-r --server-bin $bin" > $R/$name.out 2>&1
    echo "== $name exit=$?"; cat $R/$name.out
  done
done
echo IDLE_DONE
