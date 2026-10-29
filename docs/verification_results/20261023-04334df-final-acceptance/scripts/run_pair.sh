#!/bin/bash
# usage: run_pair.sh <asan|ubsan|tsan|none> <media_server-bin> <run-name> <talk|whep_pull>
E=$(cd "$(dirname "$0")/.." && pwd); kind=$1; bin=$2; name=$3; what=$4
out=$E/runs/$name
if [ -e "$out" ]; then echo "$out exists"; exit 1; fi
if pgrep -x media_server > /dev/null; then echo "stale media_server on test ports"; exit 1; fi
mkdir -p "$out"
source $E/scripts/san_env.sh $kind $out/server
if [ "$what" = talk ]; then
    python3 $E/scripts/talk.py "$bin" $E/scripts/allow_stub.py "$out/server.log" > "$out/summary.txt" 2> "$out/talk.stderr"
else
    $E/scripts/whep_pull.sh "$bin" "$out" > "$out/summary.txt" 2> "$out/whep_pull.stderr"
fi
cat "$out/summary.txt"
$E/scripts/scan_reports.sh "$out"
