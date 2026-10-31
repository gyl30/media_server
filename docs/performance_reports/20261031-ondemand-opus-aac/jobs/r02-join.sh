T=/var/tmp/ms-perf-891aec1
NS=msperf-20261013T011210Z-7788320
R=$T/runs3
for input in whip whep_pull; do
  for variant in a b; do
    name=join-$input-$variant
    bin=$([ $variant = a ] && echo build/media_server || echo build-b/media_server)
    bash $T/env_check.sh $R/$name.env.json > /dev/null || { echo "$name BLOCKED"; continue; }
    ip netns exec $NS python3 $T/aac_join.py --output $R/$name --runtime $T/rt-r --server-bin $bin --input $input --expect-restarts $([ $variant = a ] && echo 0 || echo 1) > $R/$name.out 2>&1
    echo "== $name exit=$?"; cat $R/$name.out
  done
done
echo JOIN_DONE
