#!/bin/bash
# usage: run_lifecycle.sh <asan|ubsan|tsan|none> <media_server-bin> <run-name> [lifecycle.py args]
E=$(cd "$(dirname "$0")/.." && pwd); kind=$1; bin=$2; name=$3; shift 3
out=$E/runs/$name
if [ -e "$out" ]; then echo "$out exists"; exit 1; fi
if pgrep -x media_server > /dev/null; then echo "stale media_server on test ports"; exit 1; fi
mkdir -p "$out"
source $E/scripts/san_env.sh $kind $out/server
python3 $E/scripts/lifecycle.py --bin "$bin" --stub $E/scripts/allow_stub.py --fixture $E/fixtures/perf_fixture_720p30.flv --out "$out" "$@" 2> "$out/lifecycle.stderr" | tee "$out/summary.json"
$E/scripts/scan_reports.sh "$out"
