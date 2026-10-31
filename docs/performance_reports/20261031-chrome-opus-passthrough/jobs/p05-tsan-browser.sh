T=/var/tmp/ms-perf-891aec1
NS=msperf-20261013T011210Z-7788320
R=$T/runs4
F=$R/d-fixtures
cd $T/rt-s
sha256sum san-tsan/media_server san-tsan/media_server-head-aa083a5 | cut -c1-16
for rep in 1 2; do
  for variant in head patched; do
    bin=$([ $variant = head ] && echo san-tsan/media_server-head-aa083a5 || echo san-tsan/media_server)
    out=$R/p-tsan-$variant-$rep
    rm -rf $out; mkdir -p $out
    bash $T/env_check.sh $out.env.json > /dev/null || { echo "$variant-$rep BLOCKED"; continue; }
    ip netns exec $NS env PYTHONPATH=$T/pwlib python3 $T/d_chrome.py --output $out/run --runtime $T/rt-s --server-bin $bin --fixtures $F \
      --chrome-bin $T/chrome-dl/root/opt/google/chrome/chrome --seconds 60 > $out.out 2>&1
    echo "== $variant-$rep exit=$? tsan_reports=$(grep -c '^WARNING: ThreadSanitizer' $out/run/server.log) segv=$(grep -c 'ThreadSanitizer: SEGV' $out/run/server.log)"
    grep "^SUMMARY" $out/run/server.log | sort | uniq -c
    grep -A4 "^WARNING: ThreadSanitizer" $out/run/server.log | grep -E "#[0-3] " | sed 's/(media_server.*//' | sort | uniq -c | sort -rn | head -8
    find $out -type f \( -name "*.dump" -o -name "*.mkv" \) -size +100k -delete
  done
done
echo TSAN_BROWSER_DONE
