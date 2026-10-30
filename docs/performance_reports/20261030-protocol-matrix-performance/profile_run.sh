#!/bin/bash
# usage: profile_run.sh <run-name> [perf_run args...]; records server cores 0-7 for 15 s, 20 s into the measurement window
. /var/tmp/ms-perf-891aec1/lib.sh
name=$1
shift
P=$R/prof/$name
mkdir -p $P
run $name "$@" > $P/run.txt 2>&1 &
runner=$!
until [ -e $R/$name/measurement.start ] || ! kill -0 $runner 2> /dev/null; do sleep 1; done
sleep 20
taskset -c 15 perf record -F 499 -g -C 0-7 -o $P/server.data -- sleep 15 > $P/record.log 2>&1
wait $runner
cat $P/run.txt
perf report --stdio --no-children -g none --sort dso -i $P/server.data 2> /dev/null | grep -E "^ +[0-9]" | head -25 > $P/dso.txt
perf report --stdio --no-children -g none --sort dso,sym -i $P/server.data 2> /dev/null | grep -E "^ +[0-9]" > $P/flat.txt
perf report --stdio --children -g none --sort sym -i $P/server.data 2> /dev/null | grep -E "^ +[0-9]" | head -150 > $P/children.txt
perf report --stdio --no-children -g none --sort comm -i $P/server.data 2> /dev/null | grep -E "^ +[0-9]" | head -20 > $P/comm.txt
rm -f $P/server.data.old
