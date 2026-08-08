#!/usr/bin/env bash
set -euo pipefail

server_bin="${1:-./build/media_server}"
work_dir="${2:-./network_test_output}"
mkdir -p "$work_dir"
work_dir="$(cd "$work_dir" && pwd)"
server_bin="$(realpath "$server_bin")"

main_pid=""
pull_pid=""
publish_pid=""
cleanup() {
    set +e
    [[ -n "$publish_pid" ]] && kill "$publish_pid" 2>/dev/null
    [[ -n "$pull_pid" ]] && kill "$pull_pid" 2>/dev/null
    [[ -n "$main_pid" ]] && kill "$main_pid" 2>/dev/null
    wait "$publish_pid" 2>/dev/null
    wait "$pull_pid" 2>/dev/null
    wait "$main_pid" 2>/dev/null
}
trap cleanup EXIT

wait_log() {
    local file="$1"
    local text="$2"
    for _ in $(seq 1 80); do
        if grep -Fq "$text" "$file" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
    done
    echo "missing log: $text" >&2
    cat "$file" >&2 || true
    return 1
}

probe_streams() {
    local output="$1"
    shift
    timeout 10s ffprobe -v error "$@" \
        -show_entries stream=index,codec_name,codec_type,sample_rate,channels \
        -of compact=p=0:nk=0 >"$output"
    grep -q 'codec_name=h264' "$output"
    grep -q 'codec_name=aac' "$output"
}

"$server_bin" --rtmp-port 19350 --rtsp-port 18554 --http-port 18080 \
    >"$work_dir/server.log" 2>&1 &
main_pid=$!
sleep 0.4

ffmpeg -nostdin -hide_banner -loglevel error -re \
    -f lavfi -i 'testsrc=size=320x180:rate=25' \
    -f lavfi -i 'sine=frequency=1000:sample_rate=44100' \
    -map 0:v:0 -map 1:a:0 \
    -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p \
    -g 25 -keyint_min 25 -sc_threshold 0 \
    -c:a aac -b:a 96k -ac 2 \
    -t 22 -f flv 'rtmp://127.0.0.1:19350/live/test' \
    >"$work_dir/publisher.log" 2>&1 &
publish_pid=$!

wait_log "$work_dir/server.log" '[rtmp] publish live/test'
wait_log "$work_dir/server.log" '[rtmp] input track video h264'
wait_log "$work_dir/server.log" '[rtmp] input track audio aac'
sleep 1

probe_streams "$work_dir/rtsp_from_rtmp.txt" -rtsp_transport tcp 'rtsp://127.0.0.1:18554/live/test'
probe_streams "$work_dir/rtmp_from_rtmp.txt" 'rtmp://127.0.0.1:19350/live/test'
probe_streams "$work_dir/http_flv_from_rtmp.txt" 'http://127.0.0.1:18080/live/test.flv'

# 首次请求建立共享 HLS 输出；等待自然关键帧完成切片。
curl -fsS 'http://127.0.0.1:18080/hls/live/test/index.m3u8' >"$work_dir/hls_initial.m3u8"
sleep 4
curl -fsS 'http://127.0.0.1:18080/hls/live/test/index.m3u8' >"$work_dir/hls_ready.m3u8"
segment_name="$(grep -E '^[^#].*\.ts$' "$work_dir/hls_ready.m3u8" | head -1 | sed 's#^\./##')"
[[ -n "$segment_name" ]]
curl -fsS "http://127.0.0.1:18080/hls/live/test/$segment_name" >"$work_dir/hls_segment.ts"
[[ $(( $(stat -c%s "$work_dir/hls_segment.ts") % 188 )) -eq 0 ]]
probe_streams "$work_dir/hls_from_rtmp.txt" "$work_dir/hls_segment.ts"

"$server_bin" --rtmp-port 19351 --rtsp-port 18555 --http-port 18081 \
    --rtsp-pull 'relay/test=rtsp://127.0.0.1:18554/live/test' \
    >"$work_dir/pull_server.log" 2>&1 &
pull_pid=$!

wait_log "$work_dir/pull_server.log" '[rtsp_input] connected'
wait_log "$work_dir/pull_server.log" '[rtsp_input] track video h264'
wait_log "$work_dir/pull_server.log" '[rtsp_input] track audio aac'
sleep 1

probe_streams "$work_dir/rtsp_from_rtsp.txt" -rtsp_transport tcp 'rtsp://127.0.0.1:18555/relay/test'
probe_streams "$work_dir/rtmp_from_rtsp.txt" 'rtmp://127.0.0.1:19351/relay/test'
probe_streams "$work_dir/http_flv_from_rtsp.txt" 'http://127.0.0.1:18081/relay/test.flv'

curl -fsS 'http://127.0.0.1:18081/hls/relay/test/index.m3u8' >"$work_dir/relay_hls_initial.m3u8"
sleep 4
curl -fsS 'http://127.0.0.1:18081/hls/relay/test/index.m3u8' >"$work_dir/relay_hls_ready.m3u8"
relay_segment="$(grep -E '^[^#].*\.ts$' "$work_dir/relay_hls_ready.m3u8" | head -1 | sed 's#^\./##')"
[[ -n "$relay_segment" ]]
curl -fsS "http://127.0.0.1:18081/hls/relay/test/$relay_segment" >"$work_dir/relay_hls_segment.ts"
[[ $(( $(stat -c%s "$work_dir/relay_hls_segment.ts") % 188 )) -eq 0 ]]
probe_streams "$work_dir/hls_from_rtsp.txt" "$work_dir/relay_hls_segment.ts"

kill -0 "$main_pid"
kill -0 "$pull_pid"

cat >"$work_dir/summary.txt" <<SUMMARY
rtmp input -> rtsp output: pass
rtmp input -> rtmp output: pass
rtmp input -> http-flv output: pass
rtmp input -> hls output: pass
rtsp input -> rtsp output: pass
rtsp input -> rtmp output: pass
rtsp input -> http-flv output: pass
rtsp input -> hls output: pass
both servers remained alive after client disconnects: pass
SUMMARY
cat "$work_dir/summary.txt"
