#!/bin/bash
# usage: RT=<runtime dir> gbs.sh <name> <perf_run args...>; pr2.sh plus UDP socket/memory samples every 30 s of the window
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
i=0
log=$T/runs/$name.gbs.log
while kill -0 $runner 2> /dev/null; do
  tmp=$T/runs/$name.ss.tmp
  ip netns exec $NS ss -uanmp 2> /dev/null | grep -A1 '"media_server' > $tmp
  {
    echo "t=$i $(date +%s) $(python3 $T/sssum.py $tmp | cut -d' ' -f2-)"
    echo "t=$i snmp $(ip netns exec $NS grep '^Udp:' /proc/net/snmp | tail -1)"
    echo "t=$i sockstat $(grep UDP: /proc/net/sockstat)"
    pid=$(ip netns exec $NS pgrep -x media_server | head -1)
    [ -n "$pid" ] && echo "t=$i proc fds=$(ls /proc/$pid/fd | wc -l) threads=$(ls /proc/$pid/task | wc -l) $(grep -E '^Pss:' /proc/$pid/smaps_rollup | tr -s ' ')"
  } >> $log
  if [ $i = 30 ] || [ $i = 270 ]; then gzip -c $tmp > $T/runs/$name.ss-$i.txt.gz; fi
  rm -f $tmp
  i=$((i + 30))
  sleep 30
done
wait $runner
