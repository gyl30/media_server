#!/bin/bash
T=/var/tmp/ms-perf-891aec1
while true; do
  job=$(ls $T/jobs/*.sh 2> /dev/null | head -1)
  if [ -z "$job" ]; then
    sleep 20
    continue
  fi
  name=$(basename $job .sh)
  mv $job $T/jobs-done/$name.sh
  echo "START $name $(date +%FT%T)" >> $T/queue.log
  bash $T/jobs-done/$name.sh > $T/jobs-done/$name.log 2>&1
  echo "END $name $? $(date +%FT%T)" >> $T/queue.log
done
