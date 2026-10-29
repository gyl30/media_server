#!/bin/bash
# usage: whep_lifecycle.sh <media_server-bin> <run-name>
set -u
E=/home/gyl/example/media_server/docs/performance_reports/20261029-aac-whep-capacity/san; P=/home/gyl/example/media_server/docs/performance_reports; bin=$1; name=$2
out=$E/runs/$name
[ -e "$out" ] && { echo "$out exists"; exit 1; }
pgrep -f "rtmp-port 2[13]935" > /dev/null && { echo "stale media_server"; exit 1; }
mkdir -p "$out"
python3 $P/20261028-gb-udp-sender/san/scripts/allow_stub.py & stub=$!
$bin --rtmp-port 31935 --rtsp-port 38554 --http-port 38080 --bind-address 127.0.0.1 --webrtc-address 127.0.0.1 --threads 2 \
    --signaling-url http://127.0.0.1:29191 > $out/upstream.log 2>&1 & upstream=$!
$bin --rtmp-port 21935 --rtsp-port 28554 --http-port 28080 --bind-address 127.0.0.2 --webrtc-address 127.0.0.2 --threads 4 \
    --signaling-url http://127.0.0.1:29191 > $out/server.log 2>&1 & server=$!
for i in $(seq 60); do curl -s -o /dev/null http://127.0.0.2:28080/ && curl -s -o /dev/null http://127.0.0.1:38080/ && break; sleep 1; done
F=$P/20261027-a1a3/san/fixtures/perf_fixture_720p30.flv
stream=$(openssl rand -hex 32)
pub="exec $HOME/bin/ffmpeg -nostdin -v error -re -stream_loop -1 -i $F -c copy -f flv rtmp://127.0.0.1:31935/live/$stream"
bash -c "$pub" > $out/publisher.log 2>&1 & p=$!
python3 /home/gyl/example/media_server/docs/performance_reports/20261029-whep-pull-optimize/san/scripts/whep_lifecycle.py --server 127.0.0.2:28080 --upstream 127.0.0.1:38080 --server-pid $server --upstream-pid $upstream \
    --publisher-pid $p --stream $stream --restart-publisher "$pub > $out/publisher2.log 2>&1" > $out/driver.txt 2>&1
echo "driver_rc=$?" >> $out/driver.txt
s=$(date +%s.%N); kill -TERM $server; wait $server; rc=$?
echo "server_exit=$rc shutdown_seconds=$(echo "$(date +%s.%N) - $s" | bc)" >> $out/driver.txt
kill $upstream $p $stub 2> /dev/null; wait $upstream $p 2> /dev/null
grep -E "\]\s*\[(warning|error)\]" $out/server.log | cut -c27- | sed -E "s/[0-9a-f-]{36}/<id>/g" | sort | uniq -c > $out/server-warnings.txt
cat $out/driver.txt $out/server-warnings.txt
