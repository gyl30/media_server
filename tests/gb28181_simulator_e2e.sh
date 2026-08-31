#!/usr/bin/env bash
set -euo pipefail

server_bin="${1:-./build/media_server}"
work_dir="${2:-${TMPDIR:-/tmp}/media_server-gb28181-simulator-e2e}"
ffmpeg_bin="${FFMPEG_BIN:-/home/gyl/ffmpeg901/bin/ffmpeg}"
ffprobe_bin="${FFPROBE_BIN:-/home/gyl/ffmpeg901/bin/ffprobe}"
simulator_devices="${SIMULATOR_DEVICES:-1}"
simulator_lives="${SIMULATOR_LIVES:-1}"
simulator_endpoints="${SIMULATOR_SIP_ENDPOINTS:-1}"
simulator_duration="${SIMULATOR_LIVE_DURATION:-10s}"
simulator_register_rate="${SIMULATOR_REGISTER_RATE:-200}"
simulator_register_expires="${SIMULATOR_REGISTER_EXPIRES:-120s}"
simulator_start_rate="${SIMULATOR_START_RATE:-0}"
simulator_media_profile="${SIMULATOR_MEDIA_PROFILE:-normal}"
signaling_address="${SIGNALING_ADDRESS:-127.0.0.1}"
media_server_address="${MEDIA_SERVER_ADDRESS:-127.0.0.1}"
simulator_address="${SIMULATOR_ADDRESS:-127.0.0.2}"
sample_resources="${SIMULATOR_SAMPLE_RESOURCES:-0}"
probe_attempts="${SIMULATOR_PROBE_ATTEMPTS:-100}"
signaling_bin="${SIGNALING_BIN:-}"
simulator_bin="${SIMULATOR_BIN:-}"
mkdir -p "$work_dir"
work_dir="$(cd "$work_dir" && pwd)"
server_bin="$(realpath "$server_bin")"

signaling_pid=""
media_pid=""
simulator_pid=""
sampler_pid=""

cleanup() {
    set +e
    [[ -n "$simulator_pid" ]] && kill -TERM "$simulator_pid" 2>/dev/null
    [[ -n "$sampler_pid" ]] && kill -TERM "$sampler_pid" 2>/dev/null
    [[ -n "$media_pid" ]] && kill -TERM "$media_pid" 2>/dev/null
    [[ -n "$signaling_pid" ]] && kill -TERM "$signaling_pid" 2>/dev/null
    [[ -n "$simulator_pid" ]] && wait "$simulator_pid" 2>/dev/null
    [[ -n "$sampler_pid" ]] && wait "$sampler_pid" 2>/dev/null
    [[ -n "$media_pid" ]] && wait "$media_pid" 2>/dev/null
    [[ -n "$signaling_pid" ]] && wait "$signaling_pid" 2>/dev/null
}
trap cleanup EXIT

read -r sip_port signaling_http_port rtmp_port rtsp_port media_http_port < <(python3 - "$signaling_address" "$media_server_address" <<'PY'
import socket
import sys

sockets = []
specs = [
    (sys.argv[1], socket.SOCK_DGRAM),
    (sys.argv[1], socket.SOCK_STREAM),
    (sys.argv[2], socket.SOCK_STREAM),
    (sys.argv[2], socket.SOCK_STREAM),
    (sys.argv[2], socket.SOCK_STREAM),
]
try:
    ports = []
    for address, kind in specs:
        while True:
            sock = socket.socket(socket.AF_INET, kind)
            sock.bind((address, 0))
            if kind != socket.SOCK_DGRAM or sock.getsockname()[1] < 49152:
                break
            sock.close()
        if kind == socket.SOCK_STREAM:
            sock.listen(1)
        sockets.append(sock)
        ports.append(sock.getsockname()[1])
    print(*ports)
finally:
    for sock in sockets:
        sock.close()
PY
)

wait_http() {
    local url="$1"
    local pid="$2"
    local log="$3"
    for _ in $(seq 1 100); do
        if curl --noproxy '*' -sS -o /dev/null "$url"; then
            return 0
        fi
        if ! kill -0 "$pid" 2>/dev/null; then
            cat "$log" >&2 2>/dev/null || true
            return 1
        fi
        sleep 0.05
    done
    echo "HTTP endpoint did not become ready: $url" >&2
    cat "$log" >&2 2>/dev/null || true
    return 1
}

print_logs() {
    for log in signaling media_server simulator; do
        echo "[$log]" >&2
        cat "$work_dir/$log.log" >&2 2>/dev/null || true
    done
}

case "$simulator_media_profile" in
    normal)
        media_bitrate=1100k
        media_buffer=2200k
        ;;
    high)
        media_bitrate=1500k
        media_buffer=3000k
        ;;
    *)
        echo "invalid SIMULATOR_MEDIA_PROFILE: $simulator_media_profile" >&2
        exit 2
        ;;
esac

if [[ -z "$signaling_bin" || -z "$simulator_bin" ]]; then
    (
        cd signaling
        go build -o "$work_dir/signaling" .
        go build -o "$work_dir/gb28181-simulator" ./simulator
    )
    signaling_bin="$work_dir/signaling"
    simulator_bin="$work_dir/gb28181-simulator"
else
    signaling_bin="$(realpath "$signaling_bin")"
    simulator_bin="$(realpath "$simulator_bin")"
fi

"$ffmpeg_bin" -nostdin -hide_banner -loglevel error \
    -f lavfi -i 'testsrc2=size=320x240:rate=25' -t 15 \
    -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p \
    -b:v "$media_bitrate" -minrate "$media_bitrate" -maxrate "$media_bitrate" -bufsize "$media_buffer" \
    -x264-params 'nal-hrd=cbr:force-cfr=1' \
    -g 25 -keyint_min 25 -sc_threshold 0 -bf 0 \
    -bsf:v h264_metadata=aud=insert -an -f h264 -y "$work_dir/source.h264"

"$signaling_bin" \
    --sip-listen "$signaling_address:$sip_port" \
    --sip-advertise "$signaling_address:$sip_port" \
    --http-listen "$signaling_address:$signaling_http_port" \
    >"$work_dir/signaling.log" 2>&1 &
signaling_pid=$!
wait_http "http://$signaling_address:$signaling_http_port/" "$signaling_pid" "$work_dir/signaling.log"

"$server_bin" \
    --bind-address "$media_server_address" \
    --webrtc-address "$media_server_address" \
    --rtmp-port "$rtmp_port" \
    --rtsp-port "$rtsp_port" \
    --http-port "$media_http_port" \
    --threads 2 \
    --signaling-url "http://$signaling_address:$signaling_http_port" \
    --server-id simulator-e2e \
    --control-url "http://$media_server_address:$media_http_port" \
    --media-ip "$media_server_address" \
    >"$work_dir/media_server.log" 2>&1 &
media_pid=$!
wait_http "http://$media_server_address:$media_http_port/" "$media_pid" "$work_dir/media_server.log"

"$simulator_bin" \
    --platform-sip "$signaling_address:$sip_port" \
    --control-url "http://$signaling_address:$signaling_http_port" \
    --listen "$simulator_address:0" \
    --media-bind "$simulator_address" \
    --media-profile "$simulator_media_profile" \
    --media-file "$work_dir/source.h264" \
    --ffmpeg "$ffmpeg_bin" \
    --heartbeat 2s \
    --live-duration "$simulator_duration" \
    --devices "$simulator_devices" \
    --live-count "$simulator_lives" \
    --sip-endpoints "$simulator_endpoints" \
    --register-rate "$simulator_register_rate" \
    --register-expires "$simulator_register_expires" \
    --start-rate "$simulator_start_rate" \
    >"$work_dir/simulator.log" 2>&1 &
simulator_pid=$!
if [[ "$sample_resources" == "1" ]]; then
    pidstat -h -r -u -p "$signaling_pid,$media_pid,$simulator_pid" 1 >"$work_dir/resources.pidstat" &
    sampler_pid=$!
fi

stream_url="rtsp://$media_server_address:$rtsp_port/gb/34020000001320000001/34020000001320000002"
probe_output="$work_dir/ffprobe.txt"
probe_error="$work_dir/ffprobe.err"
probe_ok=false
for _ in $(seq 1 "$probe_attempts"); do
    if timeout 3s "$ffprobe_bin" -v error -rtsp_transport tcp \
        -analyzeduration 2000000 -probesize 1000000 \
        -select_streams v:0 -show_entries stream=codec_name \
        -of default=noprint_wrappers=1:nokey=1 "$stream_url" \
        >"$probe_output" 2>"$probe_error" && [[ "$(<"$probe_output")" == "h264" ]]; then
        probe_ok=true
        break
    fi
    if ! kill -0 "$simulator_pid" 2>/dev/null; then
        break
    fi
    sleep 0.1
done

if [[ "$probe_ok" != "true" ]]; then
    echo "ffprobe did not observe H264 from $stream_url" >&2
    cat "$probe_error" >&2 2>/dev/null || true
    print_logs
    exit 1
fi

ss -H -lntup >"$work_dir/sockets.txt"
for pid in "$signaling_pid" "$media_pid" "$simulator_pid"; do
    if awk -v process="pid=$pid," 'index($0, process) && ($5 ~ /^0\.0\.0\.0:/ || $5 ~ /^\[::\]:/) {print; found=1} END {exit !found}' \
        "$work_dir/sockets.txt" >"$work_dir/any-address-sockets.txt"; then
        echo "process $pid has an any-address socket" >&2
        cat "$work_dir/any-address-sockets.txt" >&2
        exit 1
    fi
done
if ! wait "$simulator_pid"; then
    simulator_pid=""
    print_logs
    exit 1
fi
simulator_pid=""
if [[ -n "$sampler_pid" ]]; then
    kill -TERM "$sampler_pid" 2>/dev/null || true
    wait "$sampler_pid" 2>/dev/null || true
    sampler_pid=""
fi

if [[ "$simulator_devices" == "1" && "$simulator_lives" == "1" ]]; then
    grep -Fq 'simulator registered' "$work_dir/simulator.log"
    grep -Fq 'simulator Catalog sent' "$work_dir/simulator.log"
    grep -Fq 'simulator Keepalive sent' "$work_dir/simulator.log"
    grep -Fq 'simulator live started' "$work_dir/simulator.log"
    grep -Fq 'simulator live stopped' "$work_dir/simulator.log"
else
    grep -Fq 'simulator summary' "$work_dir/simulator.log"
    grep -Fq "registered=$simulator_devices" "$work_dir/simulator.log"
    grep -Fq 'live_active=0' "$work_dir/simulator.log"
fi
kill -0 "$signaling_pid"
kill -0 "$media_pid"
echo "gb28181 standalone simulator devices=$simulator_devices lives=$simulator_lives REGISTER/Digest/Catalog/Keepalive/INVITE/ACK/PS-RTP/RTSP/BYE: pass"
