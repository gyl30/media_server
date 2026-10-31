T=/var/tmp/ms-perf-891aec1
NS=msperf-20261013T011210Z-7788320
R=$T/runs4
F=$R/d-fixtures
cd $T/rt-s
for variant in p asan tsan; do
  case $variant in
    p) bin=build-p/media_server; env="";;
    asan) bin=san-asan/media_server; env="ASAN_OPTIONS=log_path=$R/p-browser-asan/sanitizer:detect_leaks=1";;
    tsan) bin=san-tsan/media_server; env="TSAN_OPTIONS=log_path=$R/p-browser-tsan/sanitizer";;
  esac
  rm -rf $R/p-browser-$variant
  bash $T/env_check.sh $R/p-browser-$variant.env.json > /dev/null || { echo "p-browser-$variant BLOCKED"; continue; }
  mkdir -p $R/p-browser-$variant
  seconds=$([ $variant = p ] && echo 90 || echo 60)
  ip netns exec $NS env $env PYTHONPATH=$T/pwlib python3 $T/d_chrome.py --output $R/p-browser-$variant/run --runtime $T/rt-s --server-bin $bin --fixtures $F \
    --chrome-bin $T/chrome-dl/root/opt/google/chrome/chrome --seconds $seconds > $R/p-browser-$variant.out 2>&1
  echo "== p-browser-$variant exit=$?"; cut -c1-1500 $R/p-browser-$variant.out
  find $R/p-browser-$variant -type f \( -name "*.dump" -o -name "*.mkv" \) -size +100k -delete
  echo "sanitizer files: $(ls $R/p-browser-$variant/ | grep -c sanitizer)"; cat $R/p-browser-$variant/sanitizer* 2>/dev/null | head -40
done
echo BROWSER_DONE
