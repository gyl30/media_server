#!/bin/bash
. /var/tmp/ms-perf-891aec1/mix.sh
name=$1; rt=$2; mix=$3; s=$4; shift 4
P=$T/prof/$name
mkdir -p $P
mixrun $rt $name $mix $s "$@" > $P/run.txt 2>&1 &
until [ -e $T/runs/$name/measurement.start ] || ! kill -0 $! 2>/dev/null; do sleep 1; done
sleep 25
taskset -c 15 perf record -F 499 -g -C 0-7 -o $P/server.data -- sleep 15 > $P/server-record.log 2>&1
wait
perf report --stdio --no-children --sort dso,sym -i $P/server.data 2>/dev/null | grep -v "^$" | grep -v "^#" | grep "%" | grep -v "^ *|" | grep -v "\-\-" | head -80 > $P/server-top.txt
perf report --stdio --no-children --sort dso -i $P/server.data 2>/dev/null | grep "%" | head -20 > $P/server-dso.txt
perf report --stdio --no-children --sort tid -i $P/server.data 2>/dev/null | grep "%" | head -20 > $P/server-tid.txt
perf report --stdio --children --sort sym -i $P/server.data 2>/dev/null | grep -E "^ +[0-9]" | head -100 > $P/server-children.txt
rm -f $P/server.data.old

perf report --stdio --no-children -g none --sort pid -i $P/server.data 2>/dev/null | grep -vE "^#|^$" | head -10 > $P/server-pid.txt
for t in $(awk '{print $2}' $P/server-pid.txt | cut -d: -f1 | head -8); do perf report --stdio --no-children -g none --tid $t --sort dso,sym -i $P/server.data 2>/dev/null | grep -E "^ +[0-9]" > $P/tid-$t.txt; done
perf report --stdio --no-children -g none --sort dso,sym -i $P/server.data 2>/dev/null | grep -E "^ +[0-9]" > $P/flat.txt
echo PROF_DONE
