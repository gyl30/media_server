#!/bin/bash
# usage: [RT=rt-q] [SERVER_BIN=build/media_server] [SERVER_ENV=...] run_perf.sh <run-name> [perf_run args...]; exit 3 = environment BLOCKED
set -u
T=/var/tmp/ms-perf-891aec1
NS=msperf-20261013T011210Z-7788320
R=$T/runs2
name=$1
shift
mkdir -p $R
if [ -e $R/$name ]; then
  echo "$name exists" >&2
  exit 2
fi
if ! bash $T/env_check.sh $R/$name.env.json > /dev/null; then
  echo "$name BLOCKED $(cat $R/$name.env.json)"
  exit 3
fi
cd $T/${RT:-rt-q}
args=$(printf '%q ' "$@")
ip netns exec $NS bash -c "ulimit -n 1048576; ${SERVER_ENV:-} ./run.sh python3 bench/perf_run.py --output $R/$name \
  --server-bin ${SERVER_BIN:-build/media_server} --client-dir build --ffmpeg-bin bin/ffmpeg --ffprobe-bin bin/ffprobe \
  --fixture .cache/perf_fixture_720p30.flv --whip-fixture .cache/perf_fixture_whip.mkv \
  --host 198.19.0.10 --client-address 198.19.0.11 --workers 8 --server-cpus 0-7 --client-cpus 8-15,24-31 \
  --min-free-memory-mib 8192 --min-free-disk-mib 4096 --max-server-pss-mib 16384 $args" > $R/$name.out 2>&1
python3 $T/sum.py $R/$name
