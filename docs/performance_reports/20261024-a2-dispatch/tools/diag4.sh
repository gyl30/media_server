#!/bin/bash
# usage: diag4.sh <name> <perf_run args...>; drain duration trace and HTTP probe, diagnostic only
set -u
T=/var/tmp/ms-perf-891aec1
NS=msperf-20261013T011210Z-7788320
name=$1
shift
P=$T/diag/$name
mkdir -p $P
$T/pr.sh $name "$@" > $P/run.txt 2>&1 &
runner=$!
until [ -e $T/runs/$name/measurement.start ] || ! kill -0 $runner 2> /dev/null; do sleep 1; done
server=$(pgrep -f "build/media_server --threads [0-9]+ --bind-address 198.19.0.10" | head -1)
bin=$(readlink -f /proc/$server/exe)
echo "server=$server bin=$bin sha=$(sha256sum $bin | cut -c1-16) start=$(date +%T)" > $P/probe.txt
sleep 10
sed "s|\$1|$bin|g" $T/drain.bt > $P/drain.bt
taskset -c 30 timeout -s INT 40 bpftrace $P/drain.bt > $P/drain.txt 2>&1 &
for i in $(seq 1 400); do
  kill -0 $runner 2> /dev/null || break
  http=$(taskset -c 31 ip netns exec $NS curl -s -o /dev/null -w "%{http_code} %{time_connect} %{time_total}" --max-time 20 http://198.19.0.10:18080/probe-$i)
  echo "$(date +%T.%N | cut -c1-12) http=[$http]" >> $P/probe.txt
  sleep 0.5
done
wait $runner
echo DIAG_DONE >> $P/probe.txt
