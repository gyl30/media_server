#!/bin/bash
# usage: RT=<runtime dir> wd.sh <name> <perf_run args...>; diagnostic WHIP socket->worker map, per-socket drops and handler durations; never used as A/B data
set -u
T=/var/tmp/ms-perf-891aec1
NS=msperf-20261013T011210Z-7788320
name=$1
shift
P=$T/prof/$name
mkdir -p $P
$T/pr2.sh d-$name "$@" > $P/run.txt 2>&1 &
runner=$!
until [ -e $T/runs/d-$name/measurement.start ]; do
  sleep 1
  kill -0 $runner 2> /dev/null || { echo "run ended before measurement" > $P/error.txt; wait $runner; exit 1; }
done
sleep 10
server=$(pgrep -f "build/media_server --threads [0-9]+ --bind-address 198.19.0.10" | head -1)
exe=$(readlink -f /proc/$server/exe)
echo "server=$server exe=$exe sha=$(sha256sum < $exe | cut -c1-64)" > $P/pids.txt
snap() {
  ip netns exec $NS ss -uanmp 2> /dev/null | grep -A1 media_server > $P/ss-$1.txt
  for t in /proc/$server/task/*; do echo "$(basename $t) $(awk '{print $14, $15, $39}' $t/stat)"; done > $P/threads-$1.txt
  date +%s.%N > $P/time-$1.txt
}
taskset -c 15 perf stat --per-thread -p $server -x "\t" --no-big-num -e cycles,instructions -o $P/perf-thread.txt -- sleep 8
snap a
taskset -c 15 bpftrace $T/wd.bt $server $exe > $P/bt.txt 2>&1
snap b
sleep 10
snap c
wait $runner
python3 $T/sum.py $T/runs/d-$name | head -1 > $P/summary.txt
echo DIAG_DONE $name
