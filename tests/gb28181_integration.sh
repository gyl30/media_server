#!/usr/bin/env bash
set -euo pipefail

server_bin="${1:-./build/media_server}"
work_dir="${2:-${TMPDIR:-/tmp}/media_server-gb28181-integration}"
server_address="${MEDIA_SERVER_ADDRESS:-127.0.0.1}"
mkdir -p "$work_dir"
work_dir="$(cd "$work_dir" && pwd)"
server_bin="$(realpath "$server_bin")"

rtmp_port=19360
rtsp_port=18564
http_port=18084
main_pid=""
publish_pid=""
rtcp_relay_pid=""

cleanup() {
    set +e
    stop_publisher
    [[ -n "$rtcp_relay_pid" ]] && kill "$rtcp_relay_pid" 2>/dev/null
    [[ -n "$main_pid" ]] && kill "$main_pid" 2>/dev/null
    [[ -n "$rtcp_relay_pid" ]] && wait "$rtcp_relay_pid" 2>/dev/null
    [[ -n "$main_pid" ]] && wait "$main_pid" 2>/dev/null
}
trap cleanup EXIT

stop_publisher() {
    if [[ -n "$publish_pid" ]]; then
        kill -INT "$publish_pid" 2>/dev/null || true
        for _ in $(seq 1 50); do
            if ! kill -0 "$publish_pid" 2>/dev/null; then
                break
            fi
            sleep 0.1
        done
        kill -KILL "$publish_pid" 2>/dev/null || true
        wait "$publish_pid" 2>/dev/null || true
        publish_pid=""
    fi
}

wait_probe_streams() {
    local output="$1"
    local video_codec="$2"
    local audio_codec="$3"
    local url="$4"

    for _ in $(seq 1 80); do
        if timeout 3s ffprobe -v error -rtsp_transport tcp \
            -show_entries stream=index,codec_name,codec_type,sample_rate,channels \
            -of compact=p=0:nk=0 "$url" >"$output" 2>/dev/null &&
            grep -q "codec_name=$video_codec" "$output" &&
            grep -q "codec_name=$audio_codec" "$output"; then
            return 0
        fi
        sleep 0.25
    done

    echo "stream probe failed: $url expected $video_codec + $audio_codec" >&2
    cat "$output" >&2 2>/dev/null || true
    cat "$work_dir/server.log" >&2 2>/dev/null || true
    return 1
}

post_gb28181_input() {
    local name="$1"
    local body="$2"
    local response="$work_dir/${name}.response"
    local code

    code="$(curl -sS -o "$response" -w '%{http_code}' -X POST \
        -H 'Content-Type: application/json' \
        --data-binary "$body" \
        "http://${server_address}:${http_port}/gb28181/create")"
    if [[ "$code" != "201" ]]; then
        echo "POST /gb28181/create returned $code" >&2
        cat "$response" >&2 || true
        cat "$work_dir/server.log" >&2 2>/dev/null || true
        return 1
    fi
    python3 - "$response" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    response = json.load(source)
assert response["result"] == "ok"
if "rtp_port" not in response:
    sys.exit(0)
rtp_port = int(response["rtp_port"])
rtcp_port = int(response["rtcp_port"])
assert rtp_port > 0 and rtp_port % 2 == 0 and rtcp_port == rtp_port + 1
print(rtp_port, rtcp_port)
PY
}

post_gb28181_output() {
    local name="$1"
    local body="$2"
    local response="$work_dir/${name}.response"
    local code

    code="$(curl -sS -o "$response" -w '%{http_code}' -X POST \
        -H 'Content-Type: application/json' \
        --data-binary "$body" \
        "http://${server_address}:${http_port}/play/gb28181/create")"
    if [[ "$code" != "201" ]]; then
        echo "POST /play/gb28181/create returned $code" >&2
        cat "$response" >&2 || true
        cat "$work_dir/server.log" >&2 2>/dev/null || true
        return 1
    fi
    [[ "$(<"$response")" == '{"result":"ok"}' ]]
}

delete_gb28181_output() {
    local name="$1"
    local body="$2"
    local response="$work_dir/${name}.response"
    local code

    code="$(curl -sS -o "$response" -w '%{http_code}' -X POST \
        -H 'Content-Type: application/json' \
        --data-binary "$body" \
        "http://${server_address}:${http_port}/play/gb28181/delete")"
    if [[ "$code" != "200" ]]; then
        echo "POST /play/gb28181/delete returned $code" >&2
        cat "$response" >&2 || true
        cat "$work_dir/server.log" >&2 2>/dev/null || true
        return 1
    fi
    [[ "$(<"$response")" == '{"result":"ok"}' ]]
}

delete_gb28181_input() {
    local name="$1"
    local body="$2"
    local allow_peer_closed="${3:-false}"
    local response="$work_dir/${name}.response"
    local code

    code="$(curl -sS -o "$response" -w '%{http_code}' -X POST \
        -H 'Content-Type: application/json' \
        --data-binary "$body" \
        "http://${server_address}:${http_port}/gb28181/delete")"
    if [[ "$code" == "500" && "$allow_peer_closed" == "true" ]]; then
        [[ "$(<"$response")" == '{"error":"operation_failed"}' ]]
        return
    fi
    if [[ "$code" != "200" ]]; then
        echo "POST /gb28181/delete returned $code" >&2
        cat "$response" >&2 || true
        cat "$work_dir/server.log" >&2 2>/dev/null || true
        return 1
    fi
    [[ "$(<"$response")" == '{"result":"ok"}' ]]
}

wait_tcp_input_shutdown() {
    local stream_name="$1"
    local first_line="$2"

    for _ in $(seq 1 50); do
        if tail -n "+$first_line" "$work_dir/server.log" | grep -Fq "gb28181 tcp session shutdown $stream_name"; then
            return 0
        fi
        sleep 0.1
    done

    echo "GB28181 TCP input did not close after peer shutdown: $stream_name" >&2
    cat "$work_dir/server.log" >&2 2>/dev/null || true
    return 1
}

assert_specific_sockets() {
    ss -H -lntup >"$work_dir/sockets.txt"
    if awk -v process="pid=$main_pid," 'index($0, process) && ($5 ~ /^0\.0\.0\.0:/ || $5 ~ /^\[::\]:/) {print; found=1} END {exit !found}' \
        "$work_dir/sockets.txt" >"$work_dir/any-address-sockets.txt"; then
        echo "media_server has an any-address socket" >&2
        cat "$work_dir/any-address-sockets.txt" >&2
        return 1
    fi
}

run_udp_case() {
    local name="$1"
    local source="$2"
    local target="$3"
    local video_codec="$4"
    local audio_codec="$5"
    local ssrc="$6"
    local output_id="$7"
    local input_body
    local output_body
    local ports
    local rtp_port
    local rtcp_port
    input_body="$(printf '{\"stream_name\":\"%s\",\"transport\":\"udp\",\"address\":\"%s\",\"payload_type\":96,\"ssrc\":%s}' \
        "$target" "$server_address" "$ssrc")"
    ports="$(post_gb28181_input "${name}_input_post" "$input_body")"
    read -r rtp_port rtcp_port <<<"$ports"
    output_body="$(printf '{\"stream_name\":\"%s\",\"output_id\":\"%s\",\"transport\":\"udp\",\"address\":\"%s\",\"rtp_port\":%s,\"rtcp_port\":%s,\"payload_type\":96,\"ssrc\":%s}' \
        "$source" "$output_id" "$server_address" "$rtp_port" "$rtcp_port" "$ssrc")"

    post_gb28181_output "${name}_output_post" "$output_body"

    wait_probe_streams "$work_dir/${name}_probe.txt" "$video_codec" "$audio_codec" \
        "rtsp://${server_address}:${rtsp_port}/${target}"
    assert_specific_sockets

    delete_gb28181_output "${name}_output_delete" "$(printf '{\"stream_name\":\"%s\",\"output_id\":\"%s\"}' "$source" "$output_id")"
    delete_gb28181_input "${name}_input_delete" "$(printf '{\"stream_name\":\"%s\"}' "$target")"
    kill -0 "$main_pid"
}

run_tcp_case() {
    local name="$1"
    local source="$2"
    local target="$3"
    local port="$4"
    local ssrc="$5"
    local output_id="$6"
    local mode="$7"
    local input_body
    local output_body

    if [[ "$mode" == "output-active" ]]; then
        input_body="$(printf '{\"stream_name\":\"%s\",\"transport\":\"tcp_passive\",\"address\":\"%s\",\"rtp_port\":%s,\"payload_type\":96,\"ssrc\":%s}' "$target" "$server_address" "$port" "$ssrc")"
        output_body="$(printf '{\"stream_name\":\"%s\",\"output_id\":\"%s\",\"transport\":\"tcp_active\",\"address\":\"%s\",\"rtp_port\":%s,\"payload_type\":96,\"ssrc\":%s}' "$source" "$output_id" "$server_address" "$port" "$ssrc")"
        post_gb28181_input "${name}_input_post" "$input_body"
        post_gb28181_output "${name}_output_post" "$output_body"
    else
        input_body="$(printf '{\"stream_name\":\"%s\",\"transport\":\"tcp_active\",\"address\":\"%s\",\"rtp_port\":%s,\"payload_type\":96,\"ssrc\":%s}' "$target" "$server_address" "$port" "$ssrc")"
        output_body="$(printf '{\"stream_name\":\"%s\",\"output_id\":\"%s\",\"transport\":\"tcp_passive\",\"address\":\"%s\",\"rtp_port\":%s,\"payload_type\":96,\"ssrc\":%s}' "$source" "$output_id" "$server_address" "$port" "$ssrc")"
        post_gb28181_output "${name}_output_post" "$output_body"
        post_gb28181_input "${name}_input_post" "$input_body"
    fi

    wait_probe_streams "$work_dir/${name}_probe.txt" h264 aac \
        "rtsp://${server_address}:${rtsp_port}/${target}"
    assert_specific_sockets

    local shutdown_log_line
    shutdown_log_line="$(($(wc -l <"$work_dir/server.log") + 1))"
    delete_gb28181_output "${name}_output_delete" "$(printf '{\"stream_name\":\"%s\",\"output_id\":\"%s\"}' "$source" "$output_id")"
    wait_tcp_input_shutdown "$target" "$shutdown_log_line"
    delete_gb28181_input "${name}_input_delete" "$(printf '{\"stream_name\":\"%s\"}' "$target")" true
    kill -0 "$main_pid"
}

MEDIA_SERVER_LOG_LEVEL=debug "$server_bin" --bind-address "$server_address" --webrtc-address "$server_address" \
    --rtmp-port "$rtmp_port" --rtsp-port "$rtsp_port" --http-port "$http_port" \
    >"$work_dir/server.log" 2>&1 &
main_pid=$!
sleep 0.5
kill -0 "$main_pid"

# H.264 + AAC source 复用到 UDP、两种 TCP 角色配对和 RTCP 验证。
ffmpeg -nostdin -hide_banner -loglevel error -re \
    -f lavfi -i 'testsrc=size=320x180:rate=25' \
    -f lavfi -i 'sine=frequency=1000:sample_rate=44100' \
    -map 0:v:0 -map 1:a:0 \
    -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p \
    -g 25 -keyint_min 25 -sc_threshold 0 \
    -c:a aac -b:a 96k -ac 2 \
    -f flv "rtmp://${server_address}:${rtmp_port}/live/gb-h264-aac" \
    >"$work_dir/publisher_h264_aac.log" 2>&1 &
publish_pid=$!
wait_probe_streams "$work_dir/source_h264_aac.txt" h264 aac \
    "rtsp://${server_address}:${rtsp_port}/live/gb-h264-aac"

run_udp_case udp_h264_aac live/gb-h264-aac relay/gb-udp-h264-aac h264 aac 100002001 udp-h264-aac
run_tcp_case tcp_output_active live/gb-h264-aac relay/gb-tcp-output-active 31100 100002004 tcp-output-active output-active
run_tcp_case tcp_output_passive live/gb-h264-aac relay/gb-tcp-output-passive 31110 100002005 tcp-output-passive output-passive

# RTCP relay 保持 output/input 对端身份独立，同时保留真实 RTP 数据路径。
rtcp_relay_rtp_port=31200
rtcp_relay_rtcp_port=31201
rtcp_status="$work_dir/rtcp_status.txt"
rtcp_sr_packet="$work_dir/rtcp_sr.bin"
rtcp_rr_packet="$work_dir/rtcp_rr.bin"
: >"$rtcp_status"
rm -f "$rtcp_sr_packet" "$rtcp_rr_packet"

rtcp_input_ports="$(post_gb28181_input rtcp_input_post \
    "$(printf '{\"stream_name\":\"relay/gb-udp-rtcp\",\"transport\":\"udp\",\"address\":\"%s\",\"payload_type\":96,\"ssrc\":100002006}' "$server_address")")"
read -r rtcp_input_rtp_port rtcp_input_rtcp_port <<<"$rtcp_input_ports"

python3 - "$rtcp_relay_rtp_port" "$rtcp_relay_rtcp_port" "$rtcp_input_rtp_port" "$rtcp_input_rtcp_port" \
    "$rtcp_status" "$rtcp_sr_packet" "$rtcp_rr_packet" "$server_address" >"$work_dir/rtcp_relay.log" 2>&1 <<'PY' &
import selectors
import socket
import sys
import time

relay_rtp_port = int(sys.argv[1])
relay_rtcp_port = int(sys.argv[2])
input_rtp_port = int(sys.argv[3])
input_rtcp_port = int(sys.argv[4])
status_path = sys.argv[5]
sr_path = sys.argv[6]
rr_path = sys.argv[7]
server_address = sys.argv[8]

rtp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
rtcp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
rtp.bind((server_address, relay_rtp_port))
rtcp.bind((server_address, relay_rtcp_port))

selector = selectors.DefaultSelector()
selector.register(rtp, selectors.EVENT_READ, "rtp")
selector.register(rtcp, selectors.EVENT_READ, "rtcp")
seen_sr = False
deadline = time.monotonic() + 40.0

with open(status_path, "a", encoding="utf-8", buffering=1) as status:
    while time.monotonic() < deadline:
        for key, _ in selector.select(timeout=1.0):
            data, peer = key.fileobj.recvfrom(65535)
            if key.data == "rtp":
                rtp.sendto(data, (server_address, input_rtp_port))
                continue
            if len(data) < 2:
                continue
            packet_type = data[1]
            if peer[1] == input_rtcp_port:
                if seen_sr and packet_type == 201:
                    with open(rr_path, "wb") as packet:
                        packet.write(data)
                    status.write("rr\n")
                    sys.exit(0)
                continue
            if packet_type == 200:
                if not seen_sr:
                    with open(sr_path, "wb") as packet:
                        packet.write(data)
                    status.write("sr\n")
                    seen_sr = True
                rtcp.sendto(data, (server_address, input_rtcp_port))

sys.exit(1)
PY
rtcp_relay_pid=$!
sleep 0.2
kill -0 "$rtcp_relay_pid"

post_gb28181_output rtcp_output_post "$(printf '{\"stream_name\":\"live/gb-h264-aac\",\"output_id\":\"udp-rtcp\",\"transport\":\"udp\",\"address\":\"%s\",\"rtp_port\":%s,\"rtcp_port\":%s,\"payload_type\":96,\"ssrc\":100002006,\"rtcp\":true}' \
    "$server_address" "$rtcp_relay_rtp_port" "$rtcp_relay_rtcp_port")"
wait_probe_streams "$work_dir/rtcp_probe.txt" h264 aac \
    "rtsp://${server_address}:${rtsp_port}/relay/gb-udp-rtcp"

if ! wait "$rtcp_relay_pid"; then
    rtcp_relay_pid=""
    echo "GB28181 RTCP SR/RR evidence timed out" >&2
    cat "$rtcp_status" >&2 || true
    cat "$work_dir/rtcp_relay.log" >&2 || true
    cat "$work_dir/server.log" >&2 || true
    exit 1
fi
rtcp_relay_pid=""
grep -qx 'sr' "$rtcp_status"
grep -qx 'rr' "$rtcp_status"
[[ -s "$rtcp_sr_packet" ]]
[[ -s "$rtcp_rr_packet" ]]
delete_gb28181_output rtcp_output_delete '{"stream_name":"live/gb-h264-aac","output_id":"udp-rtcp"}'
delete_gb28181_input rtcp_input_delete '{"stream_name":"relay/gb-udp-rtcp"}'
kill -0 "$main_pid"
stop_publisher

# 使用真实 FFmpeg RTSP publisher 覆盖生产对端常见的 G711 静态 payload 形式。
ffmpeg -nostdin -hide_banner -loglevel error -re \
    -f lavfi -i 'testsrc=size=320x180:rate=25' \
    -f lavfi -i 'sine=frequency=1200:sample_rate=8000' \
    -map 0:v:0 -map 1:a:0 \
    -c:v libx265 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -g 25 \
    -x265-params 'keyint=25:min-keyint=25:scenecut=0:bframes=0' \
    -c:a pcm_alaw -ar 8000 -ac 1 \
    -rtsp_transport tcp -f rtsp "rtsp://${server_address}:${rtsp_port}/live/gb-h265-g711a" \
    >"$work_dir/publisher_h265_g711a.log" 2>&1 &
publish_pid=$!
wait_probe_streams "$work_dir/source_h265_g711a.txt" hevc pcm_alaw \
    "rtsp://${server_address}:${rtsp_port}/live/gb-h265-g711a"
run_udp_case udp_h265_g711a live/gb-h265-g711a relay/gb-udp-h265-g711a hevc pcm_alaw 100002002 udp-h265-g711a
stop_publisher

ffmpeg -nostdin -hide_banner -loglevel error -re \
    -f lavfi -i 'testsrc=size=320x180:rate=25' \
    -f lavfi -i 'sine=frequency=1300:sample_rate=8000' \
    -map 0:v:0 -map 1:a:0 \
    -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p \
    -g 25 -keyint_min 25 -sc_threshold 0 \
    -c:a pcm_mulaw -ar 8000 -ac 1 \
    -rtsp_transport tcp -f rtsp "rtsp://${server_address}:${rtsp_port}/live/gb-h264-g711u" \
    >"$work_dir/publisher_h264_g711u.log" 2>&1 &
publish_pid=$!
wait_probe_streams "$work_dir/source_h264_g711u.txt" h264 pcm_mulaw \
    "rtsp://${server_address}:${rtsp_port}/live/gb-h264-g711u"
run_udp_case udp_h264_g711u live/gb-h264-g711u relay/gb-udp-h264-g711u h264 pcm_mulaw 100002003 udp-h264-g711u
stop_publisher

kill -0 "$main_pid"
cat >"$work_dir/summary.txt" <<'EOF_SUMMARY'
gb28181 udp h264+aac loopback: pass
gb28181 udp h265+g711a loopback: pass
gb28181 udp h264+g711u loopback: pass
gb28181 tcp output-active/input-passive h264+aac loopback: pass
gb28181 tcp output-passive/input-active h264+aac loopback: pass
gb28181 udp rtcp sender-report/receiver-report evidence: pass
EOF_SUMMARY
cat "$work_dir/summary.txt"
