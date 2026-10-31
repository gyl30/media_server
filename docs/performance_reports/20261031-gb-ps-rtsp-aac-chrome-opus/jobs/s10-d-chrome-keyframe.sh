T=/var/tmp/ms-perf-891aec1
NS=msperf-20261013T011210Z-7788320
R=$T/runs4
F=$R/d-fixtures
mkdir -p $F
cd $T/rt-s
ff() { LD_LIBRARY_PATH=lib taskset -c 8-15 bin/ffmpeg -nostdin -hide_banner -loglevel error -y "$@"; }
for variant in d; do
  rm -rf $R/d-chrome-kf-$variant
  bin=$([ $variant = a ] && echo build/media_server || echo build-d/media_server)
  bash $T/env_check.sh $R/d-chrome-kf-$variant.env.json > /dev/null || { echo "d-chrome-$variant BLOCKED"; continue; }
  ip netns exec $NS env PYTHONPATH=$T/pwlib python3 $T/d_chrome.py --output $R/d-chrome-kf-$variant --runtime $T/rt-s --server-bin $bin --fixtures $F \
    --chrome-bin $T/chrome-dl/root/opt/google/chrome/chrome --seconds 90 > $R/d-chrome-kf-$variant.out 2>&1
  echo "== d-chrome-$variant exit=$?"; cat $R/d-chrome-kf-$variant.out | cut -c1-1500
done
echo D_CHROME_DONE
