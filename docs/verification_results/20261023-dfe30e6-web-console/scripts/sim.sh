#!/bin/bash
# usage: sim.sh <run-name> [extra simulator args]   (appends pid to runs/<run>/pids)
E=$(cd "$(dirname "$0")/.." && pwd); out=$E/runs/$1; shift
setsid "$E/../../../build-go/gb-simulator" -platform-sip 127.0.0.1:35060 -listen 127.0.0.1:35062 -duration 6h -heartbeat 20s \
  -media-file "$E/gbfix/v60.h264" -audio-file "$E/gbfix/a60.alaw" "$@" >> "$out/sim.log" 2>&1 < /dev/null &
echo $! | tee -a "$out/pids"
