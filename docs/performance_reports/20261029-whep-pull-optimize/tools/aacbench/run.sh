#!/bin/bash
# usage: run.sh <input.mka> <mode>...; one perf-counted run per mode on CPU 7
cd /var/tmp/ms-perf-891aec1/aacbench
export LD_LIBRARY_PATH=/var/tmp/ms-perf-891aec1/rt-wp/lib
in=$1; shift
for mode in "$@"; do
  out=out-$(basename $in .mka)-$(echo "$mode" | tr ":,=" "__-").aac
  taskset -c 7 perf stat -x, -e cycles:u,instructions:u -o perf.tmp ./aacbench $in "$mode" $out 3 > res.tmp
  echo "$(cat res.tmp) gcycles=$(awk -F, '/cycles/{printf "%.3f", $1/1e9}' perf.tmp)"
done
