#!/usr/bin/env bash
set -euo pipefail

server_bin="${1:-./build/media_server}"
work_dir="${2:-${TMPDIR:-/tmp}/media_server-process-termination-smoke}"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
signaling_bin="${SIGNALING_BIN:-}"
worker_threads="${MEDIA_SERVER_TERMINATION_THREADS:-2}"
active_sessions="${MEDIA_SERVER_TERMINATION_ACTIVE_SESSIONS:-1}"
termination_signal="${MEDIA_SERVER_TERMINATION_SIGNAL:-TERM}"
if [[ ! "$worker_threads" =~ ^[1-9][0-9]*$ ]]; then
    echo "MEDIA_SERVER_TERMINATION_THREADS must be a positive integer" >&2
    exit 1
fi
if [[ "$active_sessions" != 0 && "$active_sessions" != 1 ]]; then
    echo "MEDIA_SERVER_TERMINATION_ACTIVE_SESSIONS must be 0 or 1" >&2
    exit 1
fi
if [[ "$termination_signal" != TERM && "$termination_signal" != INT ]]; then
    echo "MEDIA_SERVER_TERMINATION_SIGNAL must be TERM or INT" >&2
    exit 1
fi
mkdir -p "$work_dir"
work_dir="$(cd "$work_dir" && pwd)"
server_bin="$(realpath "$server_bin")"
signaling_database="$(mktemp "$work_dir/signaling.XXXXXX.db")"

http_port=18090
signaling_sip_port=15053
signaling_http_port=19093
main_pid=""
signaling_pid=""

cleanup_process()
{
    local pid="$1"
    if [[ -z "$pid" ]]; then
        return
    fi
    kill -TERM "$pid" 2>/dev/null || true
    for _ in $(seq 1 20); do
        if ! kill -0 "$pid" 2>/dev/null; then
            wait "$pid" 2>/dev/null || true
            return
        fi
        sleep 0.1
    done
    kill -KILL "$pid" 2>/dev/null || true
    for _ in $(seq 1 20); do
        if ! kill -0 "$pid" 2>/dev/null; then
            wait "$pid" 2>/dev/null || true
            return
        fi
        sleep 0.1
    done
}

cleanup()
{
    set +e
    exec 3>&- 2>/dev/null || true
    exec 3<&- 2>/dev/null || true
    exec 4>&- 2>/dev/null || true
    exec 4<&- 2>/dev/null || true
    exec 5>&- 2>/dev/null || true
    exec 5<&- 2>/dev/null || true
    exec 6>&- 2>/dev/null || true
    exec 6<&- 2>/dev/null || true
    cleanup_process "$main_pid"
    cleanup_process "$signaling_pid"
}
trap cleanup EXIT

wait_http()
{
    local url="$1"
    local pid="$2"
    local log="$3"
    for _ in $(seq 1 100); do
        if curl --noproxy '*' --connect-timeout 1 --max-time 2 -sS -o /dev/null "$url" 2>/dev/null; then
            if kill -0 "$pid" 2>/dev/null; then
                return 0
            fi
            cat "$log" >&2 2>/dev/null || true
            return 1
        fi
        if ! kill -0 "$pid" 2>/dev/null; then
            cat "$log" >&2 2>/dev/null || true
            return 1
        fi
        sleep 0.1
    done
    echo "HTTP endpoint did not become ready: $url" >&2
    cat "$log" >&2 2>/dev/null || true
    return 1
}

wait_process_exit()
{
    local pid="$1"
    local name="$2"
    local log="$3"
    for _ in $(seq 1 100); do
        if ! kill -0 "$pid" 2>/dev/null; then
            wait "$pid"
            return 0
        fi
        sleep 0.1
    done
    echo "$name did not terminate after SIGTERM" >&2
    cat "$log" >&2 2>/dev/null || true
    return 1
}

if [[ -z "$signaling_bin" ]]; then
    (
        cd "$script_dir/../signaling"
        go build -o "$work_dir/signaling" .
    )
    signaling_bin="$work_dir/signaling"
else
    signaling_bin="$(realpath "$signaling_bin")"
fi

"$signaling_bin" \
    --sip-listen "127.0.0.1:$signaling_sip_port" \
    --sip-advertise "127.0.0.1:$signaling_sip_port" \
    --http-listen "127.0.0.1:$signaling_http_port" \
    --database "$signaling_database" \
    >"$work_dir/signaling.log" 2>&1 &
signaling_pid=$!
wait_http "http://127.0.0.1:$signaling_http_port/" "$signaling_pid" "$work_dir/signaling.log"

exec 4<>"/dev/tcp/127.0.0.1/${signaling_http_port}"
printf 'GET /api/events HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n' >&4
sse_status=""
sse_content_type=""
while IFS= read -r -t 2 header <&4; do
    header="${header%$'\r'}"
    if [[ -z "$sse_status" ]]; then
        sse_status="$header"
    elif [[ "$header" == "Content-Type: text/event-stream" ]]; then
        sse_content_type="$header"
    elif [[ -z "$header" ]]; then
        break
    fi
done
if [[ "$sse_status" != "HTTP/1.1 200 OK" || -z "$sse_content_type" ]]; then
    echo "SSE endpoint did not establish: $sse_status $sse_content_type" >&2
    exit 1
fi

"$server_bin" --rtmp-port 19360 --rtsp-port 18564 --http-port "$http_port" --threads "$worker_threads" \
    --signaling-url "http://127.0.0.1:$signaling_http_port" \
    --server-id process-termination-smoke \
    --control-url "http://127.0.0.1:$http_port" \
    --media-ip 127.0.0.1 \
    >"$work_dir/server.log" 2>&1 &
main_pid=$!

wait_http "http://127.0.0.1:${http_port}/" "$main_pid" "$work_dir/server.log"

if [[ "$active_sessions" == 1 ]]; then
    exec 3<>"/dev/tcp/127.0.0.1/${http_port}"
    printf 'GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n' >&3
    wait_http "http://127.0.0.1:${http_port}/" "$main_pid" "$work_dir/server.log"

    exec 5<>"/dev/tcp/127.0.0.1/19360"
    printf '\x03' >&5
    head -c 1536 /dev/zero >&5
    rtmp_version=""
    if ! IFS= read -r -N 1 -t 2 rtmp_version <&5 || [[ "$rtmp_version" != $'\x03' ]]; then
        echo "RTMP connection did not complete the first handshake step" >&2
        exit 1
    fi

    exec 6<>"/dev/tcp/127.0.0.1/18564"
    printf 'OPTIONS * RTSP/1.0\r\nCSeq: 1\r\n\r\n' >&6
    rtsp_status=""
    while IFS= read -r -t 2 header <&6; do
        header="${header%$'\r'}"
        if [[ -z "$rtsp_status" ]]; then
            rtsp_status="$header"
        elif [[ -z "$header" ]]; then
            break
        fi
    done
    if [[ "$rtsp_status" != "RTSP/1.0 200 OK" ]]; then
        echo "RTSP connection did not complete OPTIONS: $rtsp_status" >&2
        exit 1
    fi
fi

exit_started_ns="$(date +%s%N)"
kill -"$termination_signal" "$main_pid"
wait_process_exit "$main_pid" media_server "$work_dir/server.log"
exit_elapsed_ms=$((($(date +%s%N) - exit_started_ns) / 1000000))
if ((exit_elapsed_ms > 2000)); then
    echo "media_server took ${exit_elapsed_ms}ms to exit after SIG${termination_signal}" >&2
    exit 1
fi
main_pid=""
exec 3>&-
exec 3<&-
exec 5>&-
exec 5<&-
exec 6>&-
exec 6<&-
echo "media_server SIG${termination_signal} exit: ${exit_elapsed_ms}ms"

kill -TERM "$signaling_pid"
wait_process_exit "$signaling_pid" signaling "$work_dir/signaling.log"
signaling_pid=""
exec 4>&-
exec 4<&-

python3 - "$signaling_database" <<'PY'
import sqlite3
import sys

database = sqlite3.connect(sys.argv[1])
assert database.execute("PRAGMA quick_check").fetchone() == ("ok",)
database.close()
PY
