#!/bin/bash
# usage: RT=<runtime dir> a3.sh <name> <perf_run args...>; pr2.sh plus one UDP socket memory snapshot 20 s into the window
set -u
T=/var/tmp/ms-perf-891aec1
NS=msperf-20261013T011210Z-7788320
name=$1
$T/pr2.sh "$@" &
runner=$!
until [ -e $T/runs/$name/measurement.start ]; do
  sleep 1
  kill -0 $runner 2> /dev/null || { wait $runner; exit; }
done
sleep 20
ip netns exec $NS ss -uanmp 2> /dev/null | grep -A1 media_server > $T/runs/$name.ss.txt
wait $runner
