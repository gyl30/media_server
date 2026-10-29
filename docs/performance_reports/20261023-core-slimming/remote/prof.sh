#!/bin/bash
# usage: prof.sh <run-name> <server-bin> <outputs>
T=/var/tmp/media_server_tooling/claude-20261023; name=$1; bin=$2; outs=$3
$T/ab.sh $name $bin $outs > $T/runs/$name.summary 2>&1 &
runner=$!
until [ -f $T/runs/$name/measurement.start ] || ! kill -0 $runner 2>/dev/null; do sleep 0.5; done
pid=$(pgrep -x media_server | head -1)
sleep 3
perf record -F 499 -g -p $pid -o $T/runs/$name.perf.data -- sleep 20 > /dev/null 2>&1
wait $runner
cat $T/runs/$name.summary
perf report -i $T/runs/$name.perf.data --no-children --percent-limit 0.7 --stdio -g none --sort dso,symbol 2>/dev/null | grep -v "^#" | grep -v "^$" | head -40 | awk '{$1=$1; print}' | cut -c1-110
