#!/bin/bash
# usage: RT=<runtime dir> diag5.sh <name> <perf_run args...>; drain trace from server start plus HTTP probe, diagnostic only
set -u
T=/var/tmp/ms-perf-891aec1
NS=msperf-20261013T011210Z-7788320
name=$1
shift
P=$T/diag/$name
mkdir -p $P
$T/pr2.sh $name "$@" > $P/run.txt 2>&1 &
runner=$!
server=
until [ -n "$server" ] || ! kill -0 $runner 2> /dev/null; do
  sleep 0.5
  server=$(pgrep -f "^build/media_server --threads [0-9]+ --bind-address 198.19.0.10" | head -1)
done
bin=$(readlink -f /proc/$server/exe)
echo "server=$server bin=$bin sha=$(sha256sum $bin | cut -c1-16) start=$(date +%T)" > $P/probe.txt
sed "s|\$1|$bin|g" $T/drain.bt > $P/drain.bt
taskset -c 30 timeout -s INT ${TRACE_SECONDS:-160} bpftrace $P/drain.bt > $P/drain.txt 2>&1 &
while kill -0 $runner 2> /dev/null; do
  http=$(taskset -c 31 ip netns exec $NS curl -s -o /dev/null -w "%{http_code} %{time_connect} %{time_total}" --max-time 20 http://198.19.0.10:18080/probe)
  echo "$(date +%T.%N | cut -c1-12) http=[$http]" >> $P/probe.txt
  sleep 0.5
done
wait
echo DIAG_DONE >> $P/probe.txt
