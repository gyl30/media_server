#!/bin/bash
# usage: run_perf.sh <asan|ubsan|tsan|none> <media_server-bin> <run-name> [perf_run args]
E=$(cd "$(dirname "$0")/.." && pwd); kind=$1; bin=$2; name=$3; shift 3
out=$E/runs/$name
if [ -e "$out" ]; then echo "$out exists"; exit 1; fi
mkdir -p "$out.san"
source $E/scripts/san_env.sh $kind $out.san/server
cd $E/../../../..
python3 bench/perf_run.py --output "$out" --input rtmp --sources 1 --server-bin "$bin" --client-dir build \
    --ffmpeg-bin ~/bin/ffmpeg --ffprobe-bin ~/bin/ffprobe --fixture $E/fixtures/perf_fixture_720p30.flv \
    --host 127.0.0.1 --client-address 127.0.0.2 --workers 4 --server-cpus 0-3 --client-cpus 8-23 --warmup 5 --duration 60 \
    --ready-timeout 180 --ramp-per-second 50 "$@" > "$out.out" 2>&1
tail -1 "$out.out" | cut -c1-80
grep -h "phase=peer\|phase=cleanup" $out/*.log | sort | uniq -c
$E/scripts/scan_reports.sh "$out" "$out.san"
