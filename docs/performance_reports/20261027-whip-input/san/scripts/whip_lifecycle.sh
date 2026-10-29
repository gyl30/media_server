#!/bin/bash
# usage: whip_lifecycle.sh <asan|ubsan|tsan|none> <media_server-bin> <run-name>
# One server process: repeated WHIP create/DELETE, publisher SIGKILL after and during setup, publisher stopped during setup, invalid offer,
# garbage UDP to session ports, viewer on a killed source, SIGTERM with active sessions.
set -u
E=$(cd "$(dirname "$0")/.." && pwd); kind=$1; bin=$2; name=$3
R=$(cd $E/../../../.. && pwd)
out=$E/runs/$name
[ -e "$out" ] && { echo "$out exists"; exit 1; }
pgrep -x media_server > /dev/null && { echo "stale media_server"; exit 1; }
mkdir -p "$out"
H=127.0.0.1; HTTP=28080
source $E/scripts/san_env.sh $kind $out/server
python3 $E/scripts/allow_stub.py & stub=$!
$bin --rtmp-port 21935 --rtsp-port 28554 --http-port $HTTP --bind-address $H --webrtc-address $H --threads 4 \
    --signaling-url http://$H:29191 > $out/server.log 2>&1 & server=$!
unset ASAN_OPTIONS UBSAN_OPTIONS TSAN_OPTIONS LD_LIBRARY_PATH
for i in $(seq 60); do curl -s -o /dev/null http://$H:$HTTP/ && break; sleep 1; done
P=$R/build/whip_publisher; F=$E/fixtures/perf_fixture_whip.mkv
urls() { for i in $(seq $2); do echo "http://$H:$HTTP/publish/whip/$(openssl rand -hex 32)"; done > $out/$1.urls; }
publish() { $P --whip-urls-file $out/$1.urls --fixture $F --sources $2 --bind-address 127.0.0.2 --io-threads 2 --ramp-per-second 50 --duration $3 > $out/$1.log 2>&1; }
publish_bg() { $P --whip-urls-file $out/$1.urls --fixture $F --sources $2 --bind-address 127.0.0.2 --io-threads 2 --ramp-per-second 50 --duration $3 > $out/$1.log 2>&1 & p=$!; }
for r in $(seq 15); do urls graceful-$r 8; publish graceful-$r 8 4; done
for r in 1 2; do
    urls killed-$r 8; publish_bg killed-$r 8 600; sleep 4
    stream=$(head -1 $out/killed-$r.urls | sed 's#.*/##')
    timeout 60 ~/bin/ffmpeg -nostdin -v error -i http://$H:$HTTP/$stream/$(openssl rand -hex 32).flv -c copy -f null - > $out/viewer-killed-$r.log 2>&1 & v=$!
    sleep 3; kill -9 $p; wait $p 2> /dev/null
    s=$(date +%s.%N); wait $v; echo "viewer_rc=$? ended_after=$(echo "$(date +%s.%N) - $s" | bc)" >> $out/viewer-killed-$r.log
done
urls setup-kill 16; publish_bg setup-kill 16 600; sleep 0.25; kill -9 $p; wait $p 2> /dev/null
urls setup-stop 16; publish_bg setup-stop 16 600; sleep 0.15; kill -STOP $p; sleep 40; kill -9 $p; wait $p 2> /dev/null
code=$(curl -s -o /dev/null -w "%{http_code}" -X POST -H "Content-Type: application/sdp" --data "v=0 garbage" http://$H:$HTTP/publish/whip/$(openssl rand -hex 32)); echo "invalid_offer_status=$code" > $out/invalid.txt
urls garbage 4; publish_bg garbage 4 25; sleep 6
python3 - $bin $server >> $out/invalid.txt <<'PY'
import os, random, socket, subprocess, sys
pid = sys.argv[2]
ports = sorted({int(line.split()[3].rsplit(":", 1)[1]) for line in subprocess.run(["ss", "-uanp"], capture_output=True, text=True).stdout.splitlines() if f"pid={pid}," in line})
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sent = 0
for port in ports:
    for size in (1, 12, 13, 64, 200, 1400):
        for first in (0x00, 0x01, 0x16, 0x14, 0x80, 0x81, 0xc8, 0xff):
            payload = bytes([first]) + os.urandom(size - 1)
            sock.sendto(payload, ("127.0.0.1", port)); sent += 1
print(f"garbage_ports={len(ports)} garbage_datagrams={sent}")
PY
wait $p
echo "garbage_publisher_rc=$? $(grep phase=measurement $out/garbage.log)" >> $out/invalid.txt
sleep 35
urls final 8; publish_bg final 8 600; sleep 6
s=$(date +%s.%N); kill -TERM $server; wait $server; rc=$?
echo "server_exit=$rc shutdown_seconds=$(echo "$(date +%s.%N) - $s" | bc)" > $out/shutdown.txt
kill -9 $p 2> /dev/null; wait $p 2> /dev/null; kill $stub
{
echo "created=$(grep -c 'whip session created' $out/server.log) shutdown=$(grep -c 'webrtc whip session shutdown' $out/server.log)"
echo "ice_timeout=$(grep -c 'ice activity timeout' $out/server.log) establishment_timeout=$(grep -c 'establishment timeout' $out/server.log) removed=$(grep -c 'whip session removed' $out/server.log)"
grep -h "phase=disconnect" $out/graceful-*.log | sort | uniq -c
cat $out/viewer-killed-*.log | grep viewer_rc
cat $out/invalid.txt $out/shutdown.txt
} | tee $out/summary.txt
$E/scripts/scan_reports.sh "$out"
