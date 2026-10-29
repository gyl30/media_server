#!/bin/bash
# usage: gb_lifecycle.sh <asan|ubsan|tsan|none> <media_server-bin> <run-name>
set -u
E=$(cd "$(dirname "$0")/.." && pwd); kind=$1; bin=$2; name=$3
out=$E/runs/$name
[ -e "$out" ] && { echo "$out exists"; exit 1; }
pgrep -f "rtmp-port 21935" > /dev/null && { echo "stale media_server"; exit 1; }
mkdir -p "$out"
H=127.0.0.1; HTTP=28080
source $E/scripts/san_env.sh $kind $out/server
python3 $E/scripts/allow_stub.py & stub=$!
$bin --rtmp-port 21935 --rtsp-port 28554 --http-port $HTTP --bind-address $H --webrtc-address $H --threads 4 \
    --signaling-url http://$H:29191 > $out/server.log 2>&1 & server=$!
unset ASAN_OPTIONS UBSAN_OPTIONS TSAN_OPTIONS LD_LIBRARY_PATH
for i in $(seq 60); do curl -s -o /dev/null http://$H:$HTTP/ && break; sleep 1; done
F=$E/fixtures/perf_fixture_720p30.flv
publish() { stream=$(openssl rand -hex 32); ~/bin/ffmpeg -nostdin -v error -re -stream_loop -1 -i $F -c copy -f flv rtmp://$H:21935/live/$stream > $out/publisher-$1.log 2>&1 & p=$!; }
publish 1
python3 $E/scripts/gb_lifecycle.py --http http://$H:$HTTP --stream $stream --server-pid $server --publisher-pid $p > $out/driver.txt 2>&1
echo "driver_rc=$?" >> $out/driver.txt
wait $p 2> /dev/null
publish 2
python3 $E/scripts/gb_lifecycle.py --http http://$H:$HTTP --stream $stream --server-pid $server --publisher-pid $p --phase hold > $out/hold.txt 2>&1 & d=$!
sleep 8
before=$(ss -uanp | grep -c "pid=$server,")
s=$(date +%s.%N); kill -TERM $server; wait $server; rc=$?
echo "server_exit=$rc shutdown_seconds=$(echo "$(date +%s.%N) - $s" | bc) udp_sockets_before=$before" > $out/shutdown.txt
kill $d $p $stub 2> /dev/null; wait $d $p 2> /dev/null
{
cat $out/driver.txt $out/hold.txt $out/shutdown.txt
echo "started=$(grep -c 'gb28181 udp sender started' $out/server.log) warn=$(grep -c -E '\]\s*\[(warning|error)\]' $out/server.log)"
} | tee $out/summary.txt
$E/scripts/scan_reports.sh "$out"
