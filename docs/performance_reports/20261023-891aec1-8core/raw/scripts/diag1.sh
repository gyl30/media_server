#!/bin/bash
T=/var/tmp/ms-perf-891aec1
sed "s#bench/perf_run.py#bench/perf_run_diag.py#" $T/pr.sh > $T/prd.sh; chmod +x $T/prd.sh
for i in whep_pull whip; do
  $T/prd.sh d1-$i --input $i --sources 1 --outputs flv:1,rtsp:1 --warmup 10 --duration 40 --ready-timeout 120 --ramp-per-second 100 --group-size 64 --decode-probe
done
echo DIAG1_DONE
