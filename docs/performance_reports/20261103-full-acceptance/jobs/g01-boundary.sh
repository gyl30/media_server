. /var/tmp/ms-perf-891aec1/jobs-lib/lib.sh
for run in rel-1 rel-2 rel-3 asan tsan; do
  name=g-boundary-$run
  variant=${run%%-*}
  ready $name || continue
  mkdir -p $R/$name
  cd $RT
  ip netns exec $NS env $(san_env $variant $name) PYTHONPATH=$T/pwlib python3 $T/jobs-lib/whep_boundary.py --output $R/$name/run --runtime $RT \
    --server-bin $(san_bin $variant) --chrome-bin $CHROME --seconds 60 > $R/$name.out 2>&1
  echo "== $name exit=$?"; cat $R/$name.out | cut -c1-3000
  san_report $name
done
echo BOUNDARY_DONE
