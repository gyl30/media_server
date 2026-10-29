#!/bin/bash
set -euo pipefail
umask 077
root=$(cd "$(dirname "$0")" && pwd)
runtime=/var/tmp/media_server_tooling/claude-20261023/rt-base
credential=$root/sip.private
ip=${TEST_HOST:?}
tag=${2:-run}

launch() {
  local name=$1
  shift
  setsid "$@" > "$root/$name-$tag.log" 2>&1 < /dev/null &
  echo $! > "$root/$name.pid"
  echo "$name pid $(cat "$root/$name.pid")"
}

start_media() {
  LD_LIBRARY_PATH=$runtime/lib launch media "$root/media_server" --bind-address $ip --webrtc-address $ip \
    --rtmp-port 38470 --rtsp-port 38471 --http-port 38472 --threads 2 \
    --signaling-url http://$ip:38473 --control-token "$(cat "$root/control.private")"
}

start_signaling() {
  launch signaling "$root/signaling" --database "$root/platform.db" \
    --sip-listen $ip:15060 --sip-advertise $ip:15060 --sip-id 34020000002000000001 --sip-domain 3402000000 \
    --sip-password "$(tr -d '\n' < "$credential")" --http-listen $ip:38473 \
    --media-control-url http://$ip:38472 --media-control-token "$(cat "$root/control.private")" \
    --media-ip $ip --media-http-port 38472 --media-rtmp-port 38470 --media-rtsp-port 38471
}

start_capture() {
  launch capture /usr/bin/tcpdump -U -i any -s 0 -w "$root/capture-$tag.pcap" "host ${CAMERA_HOST:?} or tcp port 38472 or tcp port 38473"
}

stop_one() {
  local name=$1 signal=${2:-TERM}
  [ -f "$root/$name.pid" ] || return 0
  local pid
  pid=$(cat "$root/$name.pid")
  if kill -0 "$pid" 2> /dev/null; then
    kill -"$signal" "$pid"
    for _ in $(seq 1 100); do
      kill -0 "$pid" 2> /dev/null || break
      sleep 0.1
    done
  fi
  if kill -0 "$pid" 2> /dev/null; then
    echo "$name pid $pid still alive"
  else
    wait "$pid" 2> /dev/null || true
    echo "$name pid $pid stopped"
    rm -f "$root/$name.pid"
  fi
}

status() {
  for name in capture media signaling; do
    if [ -f "$root/$name.pid" ] && kill -0 "$(cat "$root/$name.pid")" 2> /dev/null; then
      echo "$name running pid $(cat "$root/$name.pid")"
    else
      echo "$name not running"
    fi
  done
}

case $1 in
  start)
    [ -f "$root/control.private" ] || head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n' > "$root/control.private"
    start_capture
    sleep 1
    start_media
    sleep 1
    start_signaling
    ;;
  start-media) start_media ;;
  start-signaling) start_signaling ;;
  stop-media) stop_one media "${3:-TERM}" ;;
  stop-signaling) stop_one signaling "${3:-TERM}" ;;
  stop)
    stop_one signaling
    stop_one media
    stop_one capture INT
    ;;
  status) status ;;
esac
