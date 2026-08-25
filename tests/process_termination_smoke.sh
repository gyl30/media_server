#!/usr/bin/env bash
set -euo pipefail

server_bin="${1:-./build/media_server}"
work_dir="${2:-${TMPDIR:-/tmp}/media_server-process-termination-smoke}"
mkdir -p "$work_dir"
server_bin="$(realpath "$server_bin")"

http_port=18090
main_pid=""
client_pid=""

cleanup()
{
    set +e
    [[ -n "$client_pid" ]] && kill "$client_pid" 2>/dev/null
    [[ -n "$client_pid" ]] && wait "$client_pid" 2>/dev/null
    [[ -n "$main_pid" ]] && kill "$main_pid" 2>/dev/null
    [[ -n "$main_pid" ]] && wait "$main_pid" 2>/dev/null
}
trap cleanup EXIT

"$server_bin" --rtmp-port 19360 --rtsp-port 18564 --http-port "$http_port" --threads 2 \
    >"$work_dir/server.log" 2>&1 &
main_pid=$!

for _ in $(seq 1 100); do
    if curl -fsS "http://127.0.0.1:${http_port}/" >/dev/null 2>&1; then
        break
    fi
    if ! kill -0 "$main_pid" 2>/dev/null; then
        cat "$work_dir/server.log" >&2
        exit 1
    fi
    sleep 0.1
done

kill -0 "$main_pid"

curl --no-buffer --max-time 30 -sS "http://127.0.0.1:${http_port}/live/process-termination.flv" \
    >"$work_dir/client.out" 2>&1 &
client_pid=$!
sleep 0.2

kill -TERM "$main_pid"
for _ in $(seq 1 100); do
    if ! kill -0 "$main_pid" 2>/dev/null; then
        wait "$main_pid"
        main_pid=""
        exit 0
    fi
    sleep 0.1
done

echo "media_server did not terminate after SIGTERM" >&2
cat "$work_dir/server.log" >&2 || true
exit 1
