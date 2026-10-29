#!/bin/bash
# usage: RT=<runtime dir> prof3.sh <name> <perf_run args...>; diagnostic profiling at a fixed load, never used as A/B data
set -u
T=/var/tmp/ms-perf-891aec1
name=$1
shift
P=$T/prof/$name
mkdir -p $P
$T/pr2.sh p3-$name "$@" --warmup 15 --duration 80 > $P/run.txt 2>&1 &
runner=$!
until [ -e $T/runs/p3-$name/measurement.start ]; do
  sleep 1
  kill -0 $runner 2> /dev/null || { echo "run ended before measurement" > $P/error.txt; exit 1; }
done
sleep 15
server=$(pgrep -f "build/media_server --threads [0-9]+ --bind-address 198.19.0.10" | head -1)
echo "server=$server exe=$(readlink -f /proc/$server/exe)" > $P/pids.txt
NS=msperf-20261013T011210Z-7788320
ip netns exec $NS nstat -asz > $P/nstat-before.txt
date +%s.%N > $P/nstat-before.time
taskset -c 15 perf stat -a -C 0-7 -x '\t' --no-big-num -e cycles,instructions,context-switches,cpu-migrations,page-faults -- sleep 15 > $P/perf-stat.txt 2>&1
ip netns exec $NS nstat -asz > $P/nstat-after.txt
date +%s.%N > $P/nstat-after.time
taskset -c 15 perf record -a -C 0-7 -g -F 499 -o $P/perf.data -- sleep 15 > $P/perf-record.log 2>&1
PYTHONPATH=/usr/lib/python3/dist-packages taskset -c 15 syscount-bpfcc -p $server -d 10 -T 30 -L > $P/syscount.txt 2>&1
wait $runner
perf report --stdio --no-children --sort dso,sym -g none -i $P/perf.data 2> /dev/null | grep "%" | head -150 > $P/report-sym.txt
perf report --stdio --children --sort sym -g none -i $P/perf.data 2> /dev/null | grep "%" | head -250 > $P/report-children.txt
perf script -i $P/perf.data 2> /dev/null | gzip > $P/perf-script.txt.gz
python3 $T/sum.py $T/runs/p3-$name | head -1 > $P/summary.txt
echo PROF_DONE $name
