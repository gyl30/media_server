#!/bin/bash
# usage: ab.sh <run-name> <server-bin relative to rt-base> <outputs> [extra perf_run args]
set -u
T=/var/tmp/media_server_tooling/claude-20261023; name=$1; bin=$2; outs=$3; shift 3
mkdir -p $T/runs
ulimit -n 65536
cd $T/rt-base
ip netns exec msperf-20261013T011210Z-7788320 bash -c "ulimit -n 65536; ./run.sh python3 bench/perf_run.py --output $T/runs/$name --input rtmp --outputs $outs --sources 1 --server-bin $bin --client-dir ${CLIENT_DIR:-build} --ffmpeg-bin bin/ffmpeg --ffprobe-bin bin/ffprobe --fixture .cache/perf_fixture_720p30.flv --host 198.19.0.10 --client-address 198.19.0.11 --workers 4 --server-cpus 0-3 --client-cpus 8-15,24-31 --warmup 10 --duration 30 --ready-timeout 120 --ramp-per-second 200 $*" > $T/runs/$name.out 2>&1
python3 - "$T/runs/$name/result.json" "$name" <<'PY'
import json,sys
r=json.load(open(sys.argv[1]))
if 'process_metrics' not in r:
    print(sys.argv[2], r['status'], r.get('errors')); sys.exit()
pm=r['process_metrics']['server']; g=r['groups']
ready=sum(x['measurement']['progressing'] for x in g); req=sum(x['requested'] for x in g)
ev=r.get('server_evidence',{})
print(f"{sys.argv[2]} {r['status']} {r['capacity_boundary']} cpu_cores={pm['cpu_cores']:.4f} progressing={ready}/{req} pss_max_MiB={pm['pss_kib_max']/1024:.1f} ev={ev}")
PY
