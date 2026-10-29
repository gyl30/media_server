#!/bin/bash
# usage: prof2.sh <name> <perf_run args...>
# Diagnostic profiling at a fixed load; never used as capacity data.
set -u
T=/var/tmp/ms-perf-891aec1
name=$1
shift
P=$T/prof/$name
mkdir -p $P
$T/pr.sh p2-$name "$@" --warmup 15 --duration 100 > $P/run.txt 2>&1 &
runner=$!
until [ -e $T/runs/p2-$name/measurement.start ]; do
  sleep 1
  kill -0 $runner 2> /dev/null || { echo "run ended before measurement" > $P/error.txt; exit 1; }
done
sleep 20
server=$(pgrep -f "build/media_server --threads [0-9]+ --bind-address 198.19.0.10" | head -1)
echo "server=$server" > $P/pids.txt
cat /proc/softirqs > $P/softirqs-before.txt
date +%s.%N > $P/softirqs-before.time
taskset -c 31 perf stat -a -x '\t' -e uncore_imc/cas_count_read/,uncore_imc/cas_count_write/ -- sleep 15 > $P/perf-stat-imc.txt 2>&1 &
imc=$!
taskset -c 15 perf stat -a -C 0-7 -x '\t' --no-big-num \
  -e cycles,ref-cycles,instructions,cache-references,cache-misses,branch-misses,context-switches,cpu-migrations,page-faults \
  -- sleep 15 > $P/perf-stat.txt 2>&1
wait $imc
cat /proc/softirqs > $P/softirqs-after.txt
date +%s.%N > $P/softirqs-after.time
taskset -c 15 perf record -a -C 0-7 -g -F 499 -o $P/perf.data -- sleep 15 > $P/perf-record.log 2>&1
PYTHONPATH=/usr/lib/python3/dist-packages taskset -c 15 syscount-bpfcc -p $server -d 10 -T 40 -L > $P/syscount.txt 2>&1
PYTHONPATH=/usr/lib/python3/dist-packages taskset -c 15 python3 $T/offcputime.py -f -p $server 10 > $P/offcpu-folded.txt 2> $P/offcpu.log
wait $runner
perf report --stdio --no-children --sort dso -g none -i $P/perf.data 2> /dev/null | grep "%" > $P/report-dso.txt
perf report --stdio --no-children --sort dso,sym -g none -i $P/perf.data 2> /dev/null | grep "%" | head -150 > $P/report-sym.txt
perf report --stdio --no-children --sort comm,dso -g none -i $P/perf.data 2> /dev/null | grep "%" | head -40 > $P/report-comm.txt
perf report --stdio --children --sort sym -g none -i $P/perf.data 2> /dev/null | grep "%" | head -200 > $P/report-children.txt
perf script -i $P/perf.data 2> /dev/null | gzip > $P/perf-script.txt.gz
python3 $T/sum.py $T/runs/p2-$name | head -1 > $P/summary.txt
echo PROF_DONE $name
