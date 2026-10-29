#!/bin/bash
# usage: run_chrome.sh <asan|ubsan|tsan|none> <media_server-bin> <run-name>
E=$(cd "$(dirname "$0")/.." && pwd); kind=$1; bin=$2; name=$3
out=$E/runs/$name
if [ -e "$out" ]; then echo "$out exists"; exit 1; fi
if pgrep -x media_server > /dev/null; then echo "stale media_server on test ports"; exit 1; fi
mkdir -p "$out"
source $E/scripts/san_env.sh $kind $out/server
python3 $E/scripts/chrome_whep.py --bin "$bin" --stub $E/scripts/allow_stub.py --fixture $E/fixtures/perf_fixture_720p30.flv --out "$out" 2> "$out/chrome_whep.stderr" | tee "$out/summary.txt"
$E/scripts/scan_reports.sh "$out"
