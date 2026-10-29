#!/bin/bash
# usage: run_date.sh <asan|ubsan|tsan|none> <media_server-bin> <run-name> [rtsp_date.py args]
E=$(cd "$(dirname "$0")/.." && pwd); P=$E
kind=$1; bin=$2; out=$E/runs/$3; shift 3
if [ -e "$out" ]; then echo "$out exists"; exit 1; fi
if pgrep -x media_server > /dev/null; then echo "stale media_server"; exit 1; fi
mkdir -p "$out"
source $P/scripts/san_env.sh $kind $out/server
python3 $E/scripts/rtsp_date.py --bin "$bin" --stub $P/scripts/allow_stub.py --out "$out" "$@" 2> "$out/rtsp_date.stderr" | tee "$out/summary.txt"
$P/scripts/scan_reports.sh "$out"
