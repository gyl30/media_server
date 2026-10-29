#!/bin/bash
# usage: RT=<runtime dir> wb.sh <name> <perf_run args...>; diagnostic write-batch tracing at a fixed load, never used as A/B data
set -u
T=/var/tmp/ms-perf-891aec1
name=$1
shift
P=$T/prof/wb-$name
mkdir -p $P
$T/pr2.sh wb-$name "$@" > $P/run.txt 2>&1 &
runner=$!
until [ -e $T/runs/wb-$name/measurement.start ]; do
  sleep 1
  kill -0 $runner 2> /dev/null || { echo "run ended before measurement" > $P/error.txt; exit 1; }
done
sleep 20
server=$(pgrep -f "build/media_server --threads [0-9]+ --bind-address 198.19.0.10" | head -1)
echo "server=$server exe=$(readlink -f /proc/$server/exe)" > $P/pids.txt
taskset -c 15 perf stat -a -C 0-7 -x "\t" --no-big-num -e cycles,instructions,context-switches -- sleep 10 > $P/perf-stat.txt 2>&1
taskset -c 15 bpftrace $T/wb.bt $server > $P/wb.txt 2>&1
wait $runner
python3 $T/sum.py $T/runs/wb-$name | head -1 > $P/summary.txt
echo WB_DONE $name
