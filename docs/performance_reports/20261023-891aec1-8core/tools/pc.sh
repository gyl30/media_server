#!/bin/bash
# usage: pc.sh <search-name> <perf_capacity args...> -- [perf_run args...]
set -u
T=/var/tmp/ms-perf-891aec1
NS=msperf-20261013T011210Z-7788320
name=$1
shift
mkdir -p $T/capacity
if [ -e $T/capacity/$name ]; then
  echo "$name exists" >&2
  exit 2
fi
cd $T/rt
ip netns exec $NS bash -c "ulimit -n 1048576; ./run.sh python3 bench/perf_capacity.py --output $T/capacity/$name $* \
  --server-bin build/media_server --client-dir build --ffmpeg-bin bin/ffmpeg --ffprobe-bin bin/ffprobe \
  --fixture .cache/perf_fixture_720p30.flv --whip-fixture .cache/perf_fixture_whip.mkv \
  --host 198.19.0.10 --client-address 198.19.0.11 --workers ${WORKERS:-8} --server-cpus ${SERVER_CPUS:-0-7} --client-cpus 8-15,24-31 \
  --min-free-memory-mib 8192 --min-free-disk-mib 4096 --max-server-pss-mib 16384" > $T/capacity/$name.out 2>&1
tail -1 $T/capacity/$name.out
for round in $T/capacity/$name/round-*; do
  [ -d $round ] && python3 $T/sum.py $round | head -1 | cut -c1-400
done
