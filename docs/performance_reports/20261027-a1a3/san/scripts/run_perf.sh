#!/bin/bash
# usage: run_perf.sh <asan|ubsan|tsan|none> <media_server-bin> <run-name> <input> <outputs> [perf_run args]
E=$(cd "$(dirname "$0")/.." && pwd); kind=$1; bin=$2; name=$3; input=$4; outs=$5; shift 5
out=$E/runs/$name
if [ -e "$out" ]; then echo "$out exists"; exit 1; fi
mkdir -p "$out.san"
source $E/scripts/san_env.sh $kind $out.san/server
cd $E/../../../..
python3 bench/perf_run.py --output "$out" --input $input --outputs $outs --sources 1 --server-bin "$bin" --client-dir build \
    --ffmpeg-bin ~/bin/ffmpeg --ffprobe-bin ~/bin/ffprobe --fixture $E/fixtures/perf_fixture_720p30.flv \
    --whip-fixture $E/fixtures/perf_fixture_whip.mkv --host 127.0.0.1 --client-address 127.0.0.2 --workers 4 \
    --server-cpus 0-3 --client-cpus 8-23 --warmup 5 --duration 20 --ready-timeout 180 --ramp-per-second 50 "$@" > "$out.out" 2>&1
python3 - "$out/result.json" "$name" <<'PY'
import json, sys
try:
    r = json.load(open(sys.argv[1]))
except (OSError, ValueError) as error:
    print(sys.argv[2], "NO_RESULT", error); sys.exit()
groups = r.get("groups", [])
progressing = sum(g.get("measurement", {}).get("progressing", 0) for g in groups)
requested = sum(g.get("requested", 0) for g in groups)
server = [p for p in r.get("processes", []) if p.get("name") == "server"]
print(f"{sys.argv[2]} {r.get('status')} {r.get('capacity_boundary')} progressing={progressing}/{requested} "
      f"server_exit={server[0]['exit_code'] if server else 'n/a'} fault={r.get('fault', {}).get('status') if isinstance(r.get('fault'), dict) else r.get('fault')} errors={r.get('errors')}")
PY
$E/scripts/scan_reports.sh "$out" "$out.san"
