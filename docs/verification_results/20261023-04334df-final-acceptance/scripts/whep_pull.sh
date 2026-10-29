#!/bin/bash
# usage: whep_pull.sh <media_server-bin> <out-dir> : B publishes via RTMP, A pulls B over WHEP, ffmpeg decodes A
E=$(cd "$(dirname "$0")/.." && pwd); bin=$1; D=$2
T=$(python3 -c "import secrets;print(secrets.token_hex(32))"); P=$(python3 -c "import secrets;print(secrets.token_hex(32))"); X=$(python3 -c "import secrets;print(secrets.token_hex(32))")
python3 $E/scripts/allow_stub.py & stub=$!
$bin --threads 4 --rtmp-port 21935 --rtsp-port 28554 --http-port 28080 --bind-address 127.0.0.1 --webrtc-address 127.0.0.1 --signaling-url http://127.0.0.1:29191 > $D/b.log 2>&1 & b=$!
$bin --threads 4 --rtmp-port 11935 --rtsp-port 18554 --http-port 18080 --bind-address 127.0.0.2 --webrtc-address 127.0.0.2 --signaling-url http://127.0.0.1:29191 > $D/a.log 2>&1 & a=$!
sleep 1
~/bin/ffmpeg -nostdin -v error -re -stream_loop -1 -i $E/fixtures/perf_fixture_720p30.flv -c copy -f flv rtmp://127.0.0.1:21935/live/$T > $D/pub.log 2>&1 & pub=$!
sleep 3
curl -s -w " create=%{http_code}\n" -H 'Content-Type: application/json' -XPOST http://127.0.0.2:18080/pull/create -d "{\"stream_id\":\"$X\",\"url\":\"http://127.0.0.1:28080/play/whep/$T/$P\"}"
sleep 4
for proto in rtsp flv; do
  case $proto in
    rtsp) url="rtsp://127.0.0.2:18554/$X/$P"; extra="-rtsp_transport tcp";;
    flv) url="http://127.0.0.2:18080/$X/$P.flv"; extra="";;
  esac
  ~/bin/ffmpeg -nostdin -v error $extra -i "$url" -t 8 -map 0 -f null - -progress pipe:1 > $D/pull-$proto.out 2>&1
  echo "$proto: $(grep -E '^frame=' $D/pull-$proto.out | tail -1) errors=$(grep -vcE '=' $D/pull-$proto.out)"
done
kill -KILL $pub; wait $pub 2>/dev/null
sleep 3
echo "after_source_stop: $(grep -c -E 'whep pull.*(end|closed|stopped)|source stream ended' $D/a.log $D/b.log | tr '\n' ' ')"
kill -TERM $a $b; wait $a; echo "a_exit=$?"; wait $b; echo "b_exit=$?"
kill $stub; wait $stub 2>/dev/null
