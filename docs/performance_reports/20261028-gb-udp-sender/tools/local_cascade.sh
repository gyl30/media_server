#!/bin/bash
# usage: local_cascade.sh <media_server-bin> <run-name> <sources> <duration> [perf_run args]; upstream media_server GB UDP sender -> tested GB UDP receiver
set -u
E=$(cd "$(dirname "$0")/.." && pwd); bin=$1; name=$2; n=$3; duration=$4; shift 4
out=$E/raw/local/$name
F=$E/../20261027-a1a3/san/fixtures
if [ -e "$out" ]; then echo "$out exists"; exit 1; fi
cd $E/../../..
python3 bench/perf_run.py --output "$out" --input gb_udp --sources $n --outputs flv:$n --server-bin "$bin" --client-dir build \
    --ffmpeg-bin ~/bin/ffmpeg --ffprobe-bin ~/bin/ffprobe --fixture $F/perf_fixture_720p30.flv \
    --host 127.0.0.1 --client-address 127.0.0.2 --workers 4 --server-cpus 0-3 --client-cpus 8-23 \
    --warmup 5 --duration $duration --ready-timeout 180 --ramp-per-second 100 "$@" > "$out.out" 2>&1 &
runner=$!
until [ -e $out/measurement.start ]; do sleep 1; kill -0 $runner 2> /dev/null || break; done
i=0
while kill -0 $runner 2> /dev/null; do
  ss -uanmp 2> /dev/null | grep -A1 '"media_server' > $out/ss-$(printf %03d $i).txt
  i=$((i + 10)); sleep 10
done
wait $runner
for f in $(ls $out/ss-*.txt | sed -n '1p;$p'); do python3 $E/tools/sssum.py --address 127.0.0.2 $f; done
