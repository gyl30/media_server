#!/usr/bin/env bash
set -euo pipefail

server_bin="${1:-./build/media_server}"
work_dir="${2:-${TMPDIR:-/tmp}/media_server-process-termination-smoke}"
mkdir -p "$work_dir"
server_bin="$(realpath "$server_bin")"

http_port=18090
main_pid=""

cleanup()
{
    set +e
    exec 3>&- 2>/dev/null || true
    exec 3<&- 2>/dev/null || true
    [[ -n "$main_pid" ]] && kill "$main_pid" 2>/dev/null
    [[ -n "$main_pid" ]] && wait "$main_pid" 2>/dev/null
}
trap cleanup EXIT

"$server_bin" --rtmp-port 19360 --rtsp-port 18564 --http-port "$http_port" --threads 2 \
    >"$work_dir/server.log" 2>&1 &
main_pid=$!

ready=0
for _ in $(seq 1 100); do
    if curl --connect-timeout 1 --max-time 2 -sS -o /dev/null "http://127.0.0.1:${http_port}/"; then
        ready=1
        break
    fi
    if ! kill -0 "$main_pid" 2>/dev/null; then
        cat "$work_dir/server.log" >&2
        exit 1
    fi
    sleep 0.1
done

if [[ "$ready" -ne 1 ]]; then
    echo "http server did not become ready" >&2
    cat "$work_dir/server.log" >&2 || true
    exit 1
fi

exec 3<>"/dev/tcp/127.0.0.1/${http_port}"
printf 'GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n' >&3

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
