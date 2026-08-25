#!/usr/bin/env bash
set -euo pipefail

server_bin="${1:-./build/media_server}"
work_dir="${2:-${TMPDIR:-/tmp}/media_server-gb28181-integration}"
mkdir -p "$work_dir"
work_dir="$(cd "$work_dir" && pwd)"
server_bin="$(realpath "$server_bin")"

rtmp_port=19360
rtsp_port=18564
http_port=18084
remote_rtcp_sink_port=31999

main_pid=""
publish_pid=""
rtcp_sink_pid=""
rtcp_relay_pid=""

cleanup() {
    set +e
    [[ -n "$publish_pid" ]] && kill "$publish_pid" 2>/dev/null
    [[ -n "$rtcp_relay_pid" ]] && kill "$rtcp_relay_pid" 2>/dev/null
    [[ -n "$rtcp_sink_pid" ]] && kill "$rtcp_sink_pid" 2>/dev/null
    [[ -n "$main_pid" ]] && kill "$main_pid" 2>/dev/null
    [[ -n "$publish_pid" ]] && wait "$publish_pid" 2>/dev/null
    [[ -n "$rtcp_relay_pid" ]] && wait "$rtcp_relay_pid" 2>/dev/null
    [[ -n "$rtcp_sink_pid" ]] && wait "$rtcp_sink_pid" 2>/dev/null
    [[ -n "$main_pid" ]] && wait "$main_pid" 2>/dev/null
}
trap cleanup EXIT

stop_publisher() {
    if [[ -n "$publish_pid" ]]; then
        kill "$publish_pid" 2>/dev/null || true
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
    local target="$2"
    local query="$3"
    local response="$work_dir/${name}.response"
    local code

    code="$(curl -sS -o "$response" -w '%{http_code}' -X POST \
        "http://127.0.0.1:${http_port}/gb28181/${target}?${query}")"
    if [[ "$code" != "201" ]]; then
        echo "POST /gb28181/${target} returned $code" >&2
        cat "$response" >&2 || true
        cat "$work_dir/server.log" >&2 2>/dev/null || true
        return 1
    fi
}

post_gb28181_output() {
    local name="$1"
    local source="$2"
    local output_id="$3"
    local query="$4"
    local response="$work_dir/${name}.response"
    local code

    code="$(curl -sS -o "$response" -w '%{http_code}' -X POST \
        "http://127.0.0.1:${http_port}/gb28181/output/${source}?output_id=${output_id}&${query}")"
    if [[ "$code" != "201" ]]; then
        echo "POST /gb28181/output/${source} returned $code" >&2
        cat "$response" >&2 || true
        cat "$work_dir/server.log" >&2 2>/dev/null || true
        return 1
    fi
}

delete_session() {
    local name="$1"
    local path="$2"
    local response="$work_dir/${name}.response"
    local code

    code="$(curl -sS -o "$response" -w '%{http_code}' -X DELETE \
        "http://127.0.0.1:${http_port}${path}")"
    if [[ "$code" != "204" ]]; then
        echo "DELETE $path returned $code" >&2
        cat "$response" >&2 || true
        cat "$work_dir/server.log" >&2 2>/dev/null || true
        return 1
    fi
}

run_udp_case() {
    local name="$1"
    local source="$2"
    local target="$3"
    local video_codec="$4"
    local audio_codec="$5"
    local rtp_port="$6"
    local rtcp_port="$7"
    local ssrc="$8"
    local output_id="$9"
    local input_query="transport=udp&address=127.0.0.1&rtp_port=${rtp_port}&rtcp_port=${rtcp_port}&payload_type=96&ssrc=${ssrc}&remote_rtcp_port=${remote_rtcp_sink_port}"
    local output_query="transport=udp&address=127.0.0.1&rtp_port=${rtp_port}&rtcp_port=${rtcp_port}&payload_type=96&ssrc=${ssrc}"

    post_gb28181_input "${name}_input_post" "$target" "$input_query"
    post_gb28181_output "${name}_output_post" "$source" "$output_id" "$output_query"

    wait_probe_streams "$work_dir/${name}_probe.txt" "$video_codec" "$audio_codec" \
        "rtsp://127.0.0.1:${rtsp_port}/${target}"

    delete_session "${name}_output_delete" "/gb28181/output/${source}?output_id=${output_id}"
    delete_session "${name}_input_delete" "/gb28181/${target}"
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
    local input_query
    local output_query

    if [[ "$mode" == "output-active" ]]; then
        input_query="transport=tcp_passive&address=0.0.0.0&rtp_port=${port}&payload_type=96&ssrc=${ssrc}"
        output_query="transport=tcp_active&address=127.0.0.1&rtp_port=${port}&payload_type=96&ssrc=${ssrc}"
        post_gb28181_input "${name}_input_post" "$target" "$input_query"
        post_gb28181_output "${name}_output_post" "$source" "$output_id" "$output_query"
    else
        input_query="transport=tcp_active&address=127.0.0.1&rtp_port=${port}&payload_type=96&ssrc=${ssrc}"
        output_query="transport=tcp_passive&address=0.0.0.0&rtp_port=${port}&payload_type=96&ssrc=${ssrc}"
        post_gb28181_output "${name}_output_post" "$source" "$output_id" "$output_query"
        post_gb28181_input "${name}_input_post" "$target" "$input_query"
    fi

    wait_probe_streams "$work_dir/${name}_probe.txt" h264 aac \
        "rtsp://127.0.0.1:${rtsp_port}/${target}"

    delete_session "${name}_output_delete" "/gb28181/output/${source}?output_id=${output_id}"

    local input_delete_response="$work_dir/${name}_input_delete.response"
    local input_delete_code
    input_delete_code="$(curl -sS -o "$input_delete_response" -w '%{http_code}' -X DELETE \
        "http://127.0.0.1:${http_port}/gb28181/${target}")"
    if [[ "$input_delete_code" != "204" && "$input_delete_code" != "404" ]]; then
        echo "DELETE /gb28181/${target} returned $input_delete_code" >&2
        cat "$input_delete_response" >&2 || true
        cat "$work_dir/server.log" >&2 2>/dev/null || true
        return 1
    fi
    if [[ "$input_delete_code" == "404" ]] && ! grep -qx 'gb28181 session not found' "$input_delete_response"; then
        echo "DELETE /gb28181/${target} returned unexpected 404 response" >&2
        cat "$input_delete_response" >&2 || true
        cat "$work_dir/server.log" >&2 2>/dev/null || true
        return 1
    fi
    kill -0 "$main_pid"
}

python3 - "$remote_rtcp_sink_port" >"$work_dir/rtcp_sink.log" 2>&1 <<'PY' &
import socket
import sys

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("127.0.0.1", int(sys.argv[1])))
while True:
    sock.recvfrom(65535)
PY
rtcp_sink_pid=$!

"$server_bin" --rtmp-port "$rtmp_port" --rtsp-port "$rtsp_port" --http-port "$http_port" \
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
    -f flv "rtmp://127.0.0.1:${rtmp_port}/live/gb-h264-aac" \
    >"$work_dir/publisher_h264_aac.log" 2>&1 &
publish_pid=$!
wait_probe_streams "$work_dir/source_h264_aac.txt" h264 aac \
    "rtsp://127.0.0.1:${rtsp_port}/live/gb-h264-aac"

run_udp_case udp_h264_aac live/gb-h264-aac relay/gb-udp-h264-aac h264 aac 31000 31001 0100002001 udp-h264-aac
run_tcp_case tcp_output_active live/gb-h264-aac relay/gb-tcp-output-active 31100 0100002004 tcp-output-active output-active
run_tcp_case tcp_output_passive live/gb-h264-aac relay/gb-tcp-output-passive 31110 0100002005 tcp-output-passive output-passive

# RTCP relay 保持 output/input 对端身份独立，同时保留真实 RTP 数据路径。
rtcp_relay_rtp_port=31200
rtcp_relay_rtcp_port=31201
rtcp_input_rtp_port=31210
rtcp_input_rtcp_port=31211
rtcp_status="$work_dir/rtcp_status.txt"
rtcp_sr_packet="$work_dir/rtcp_sr.bin"
rtcp_rr_packet="$work_dir/rtcp_rr.bin"
: >"$rtcp_status"
rm -f "$rtcp_sr_packet" "$rtcp_rr_packet"

python3 - "$rtcp_relay_rtp_port" "$rtcp_relay_rtcp_port" "$rtcp_input_rtp_port" "$rtcp_input_rtcp_port" \
    "$rtcp_status" "$rtcp_sr_packet" "$rtcp_rr_packet" >"$work_dir/rtcp_relay.log" 2>&1 <<'PY' &
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

rtp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
rtcp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
rtp.bind(("127.0.0.1", relay_rtp_port))
rtcp.bind(("127.0.0.1", relay_rtcp_port))

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
                rtp.sendto(data, ("127.0.0.1", input_rtp_port))
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
                rtcp.sendto(data, ("127.0.0.1", input_rtcp_port))

sys.exit(1)
PY
rtcp_relay_pid=$!
sleep 0.2
kill -0 "$rtcp_relay_pid"

post_gb28181_input rtcp_input_post relay/gb-udp-rtcp \
    "transport=udp&address=127.0.0.1&rtp_port=${rtcp_input_rtp_port}&rtcp_port=${rtcp_input_rtcp_port}&payload_type=96&ssrc=0100002006&remote_rtp_address=127.0.0.1&remote_rtp_port=${rtcp_relay_rtp_port}&remote_rtcp_port=${rtcp_relay_rtcp_port}"
post_gb28181_output rtcp_output_post live/gb-h264-aac udp-rtcp \
    "transport=udp&address=127.0.0.1&rtp_port=${rtcp_relay_rtp_port}&rtcp_port=${rtcp_relay_rtcp_port}&payload_type=96&ssrc=0100002006&rtcp=1"
wait_probe_streams "$work_dir/rtcp_probe.txt" h264 aac \
    "rtsp://127.0.0.1:${rtsp_port}/relay/gb-udp-rtcp"

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
delete_session rtcp_output_delete "/gb28181/output/live/gb-h264-aac?output_id=udp-rtcp"
delete_session rtcp_input_delete "/gb28181/relay/gb-udp-rtcp"
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
    -rtsp_transport tcp -f rtsp "rtsp://127.0.0.1:${rtsp_port}/live/gb-h265-g711a" \
    >"$work_dir/publisher_h265_g711a.log" 2>&1 &
publish_pid=$!
wait_probe_streams "$work_dir/source_h265_g711a.txt" hevc pcm_alaw \
    "rtsp://127.0.0.1:${rtsp_port}/live/gb-h265-g711a"
run_udp_case udp_h265_g711a live/gb-h265-g711a relay/gb-udp-h265-g711a hevc pcm_alaw 31010 31011 0100002002 udp-h265-g711a
stop_publisher

ffmpeg -nostdin -hide_banner -loglevel error -re \
    -f lavfi -i 'testsrc=size=320x180:rate=25' \
    -f lavfi -i 'sine=frequency=1300:sample_rate=8000' \
    -map 0:v:0 -map 1:a:0 \
    -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p \
    -g 25 -keyint_min 25 -sc_threshold 0 \
    -c:a pcm_mulaw -ar 8000 -ac 1 \
    -rtsp_transport tcp -f rtsp "rtsp://127.0.0.1:${rtsp_port}/live/gb-h264-g711u" \
    >"$work_dir/publisher_h264_g711u.log" 2>&1 &
publish_pid=$!
wait_probe_streams "$work_dir/source_h264_g711u.txt" h264 pcm_mulaw \
    "rtsp://127.0.0.1:${rtsp_port}/live/gb-h264-g711u"
run_udp_case udp_h264_g711u live/gb-h264-g711u relay/gb-udp-h264-g711u h264 pcm_mulaw 31020 31021 0100002003 udp-h264-g711u
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
