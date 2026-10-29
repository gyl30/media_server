#!/bin/bash
# usage: pull_race.sh <media_server-bin> <run-name>   (source san_env.sh first for sanitizer builds)
set -u
E=$(cd "$(dirname "$0")/.." && pwd); bin=$1; name=$2
out=$E/runs/$name
[ -e "$out" ] && { echo "$out exists"; exit 1; }
mkdir -p "$out"
python3 $E/../../20261028-gb-udp-sender/san/scripts/allow_stub.py & stub=$!
$bin --rtmp-port 21935 --rtsp-port 28554 --http-port 28080 --bind-address 127.0.0.2 --webrtc-address 127.0.0.2 --threads 4 \
    --signaling-url http://127.0.0.1:29191 > $out/server.log 2>&1 & server=$!
for i in $(seq 60); do curl -s -o /dev/null http://127.0.0.2:28080/ && break; sleep 1; done
python3 $E/scripts/pull_create_delete_race.py --server 127.0.0.2:28080 --seconds ${SECONDS_RUN:-20} --pairs ${PAIRS:-4} > $out/driver.txt 2>&1
kill -TERM $server; wait $server; echo "server_exit=$?" >> $out/driver.txt
kill $stub 2> /dev/null
cat $out/driver.txt
