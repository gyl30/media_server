#!/bin/bash
# usage: env.sh up <run-name> | down <run-name>
# upstream media_server (127.0.0.2, allow stub) feeds pull sources; DUT = media_server 127.0.0.1 + signaling + GB simulator
set -u
E=$(cd "$(dirname "$0")/.." && pwd); R=$(cd "$E/../../.." && pwd)
cmd=$1; out=$E/runs/$2
UP=$(cat "$E/scripts/upstream_stream" 2>/dev/null)
if [ "$cmd" = down ]; then
  for p in $(tac "$out/pids" 2>/dev/null); do kill -TERM "$p" 2>/dev/null; done
  for i in $(seq 1 100); do alive=0; for p in $(cat "$out/pids"); do kill -0 "$p" 2>/dev/null && alive=1; done; [ $alive = 0 ] && break; sleep 0.2; done
  for p in $(cat "$out/pids"); do kill -0 "$p" 2>/dev/null && { echo "still alive $p"; kill -KILL "$p"; }; done
  exit 0
fi
[ -e "$out" ] && { echo "$out exists"; exit 1; }
pgrep -x media_server >/dev/null && { echo "stale media_server"; exit 1; }
pgrep -x signaling >/dev/null && { echo "stale signaling"; exit 1; }
mkdir -p "$out"; : > "$out/pids"
start() { name=$1; shift; setsid "$@" > "$out/$name.log" 2>&1 < /dev/null & echo $! >> "$out/pids"; }
start stub python3 "$E/../20261023-04334df-final-acceptance/scripts/allow_stub.py"
start upstream "$R/build/media_server" --bind-address 127.0.0.2 --webrtc-address 127.0.0.2 --rtmp-port 21935 --rtsp-port 28554 --http-port 28080 --threads 2 --signaling-url http://127.0.0.1:29191
sleep 1
start publisher "$HOME/bin/ffmpeg" -nostdin -v error -re -stream_loop -1 -i "$E/fixtures/perf_fixture_720p30.flv" -c copy -f flv "rtmp://127.0.0.2:21935/live/$UP"
start media "$R/build/media_server" --bind-address 127.0.0.1 --webrtc-address 127.0.0.1 --rtmp-port 31935 --rtsp-port 38554 --http-port 38080 --threads 4 --signaling-url http://127.0.0.1:39090
start signaling "$R/build-go/signaling" -database "$out/signaling.db" -http-listen 127.0.0.1:39090 -sip-listen 127.0.0.1:35060 -sip-advertise 127.0.0.1:35060 -media-control-url http://127.0.0.1:38080 -media-ip 127.0.0.1 -media-http-port 38080 -media-rtmp-port 31935 -media-rtsp-port 38554
for i in $(seq 1 50); do curl -fs http://127.0.0.1:39090/api/devices >/dev/null && break; sleep 0.2; done
echo "up: console http://127.0.0.1:39090/  upstream stream $UP"
