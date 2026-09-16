#!/usr/bin/env bash
set -euo pipefail

server_bin="${1:-./build/media_server}"
work_dir="${2:-./network_test_output}"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
signaling_bin="${SIGNALING_BIN:-}"
mkdir -p "$work_dir"
work_dir="$(cd "$work_dir" && pwd)"
server_bin="$(realpath "$server_bin")"

main_signaling_sip_port=15050
main_signaling_http_port=19090
av1_signaling_sip_port=15051
av1_signaling_http_port=19091
pull_signaling_sip_port=15052
pull_signaling_http_port=19092
main_signaling_database="$(mktemp "$work_dir/main_signaling.XXXXXX.db")"
av1_signaling_database="$(mktemp "$work_dir/av1_signaling.XXXXXX.db")"
pull_signaling_database="$(mktemp "$work_dir/pull_signaling.XXXXXX.db")"

ffprobe_version="$(ffprobe -version | head -1)"
ffprobe_major="$(sed -n 's/^ffprobe version n\?\([0-9][0-9]*\).*/\1/p' <<<"$ffprobe_version")"
if [[ -z "$ffprobe_major" || "$ffprobe_major" -lt 8 ]]; then
    echo "ffprobe 8 or newer is required for AV1 RTP integration tests: $ffprobe_version" >&2
    exit 1
fi

main_pid=""
main_signaling_pid=""
pull_signaling_pid=""
pull_pid=""
publish_pid=""
rtsp_publish_pid=""
av1_server_pid=""
av1_publish_pid=""
av1_signaling_pid=""
cleanup() {
    set +e
    [[ -n "$av1_publish_pid" ]] && kill -TERM "$av1_publish_pid" 2>/dev/null
    [[ -n "$rtsp_publish_pid" ]] && kill -TERM "$rtsp_publish_pid" 2>/dev/null
    [[ -n "$publish_pid" ]] && kill -TERM "$publish_pid" 2>/dev/null
    [[ -n "$av1_server_pid" ]] && kill -TERM "$av1_server_pid" 2>/dev/null
    [[ -n "$pull_pid" ]] && kill -TERM "$pull_pid" 2>/dev/null
    [[ -n "$main_pid" ]] && kill -TERM "$main_pid" 2>/dev/null
    [[ -n "$av1_signaling_pid" ]] && kill -TERM "$av1_signaling_pid" 2>/dev/null
    [[ -n "$pull_signaling_pid" ]] && kill -TERM "$pull_signaling_pid" 2>/dev/null
    [[ -n "$main_signaling_pid" ]] && kill -TERM "$main_signaling_pid" 2>/dev/null
    [[ -n "$av1_publish_pid" ]] && wait "$av1_publish_pid" 2>/dev/null
    [[ -n "$rtsp_publish_pid" ]] && wait "$rtsp_publish_pid" 2>/dev/null
    [[ -n "$publish_pid" ]] && wait "$publish_pid" 2>/dev/null
    [[ -n "$av1_server_pid" ]] && wait "$av1_server_pid" 2>/dev/null
    [[ -n "$pull_pid" ]] && wait "$pull_pid" 2>/dev/null
    [[ -n "$main_pid" ]] && wait "$main_pid" 2>/dev/null
    [[ -n "$av1_signaling_pid" ]] && wait "$av1_signaling_pid" 2>/dev/null
    [[ -n "$pull_signaling_pid" ]] && wait "$pull_signaling_pid" 2>/dev/null
    [[ -n "$main_signaling_pid" ]] && wait "$main_signaling_pid" 2>/dev/null
}
trap cleanup EXIT

wait_http() {
    local url="$1"
    local pid="$2"
    local log="$3"
    for _ in $(seq 1 100); do
        if curl --noproxy '*' -sS --max-time 1 -o /dev/null "$url" 2>/dev/null; then
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
        sleep 0.05
    done
    echo "HTTP endpoint did not become ready: $url" >&2
    cat "$log" >&2 2>/dev/null || true
    return 1
}

allocate_publish() {
    local label="$1"
    local signaling_port="$2"
    local protocol="$3"
    local stream_name="$4"
    local response="$work_dir/${label}_allocation.json"
    local body
    local status
    printf -v body '{"protocol":"%s","stream_name":"%s"}' "$protocol" "$stream_name"
    status="$(curl --noproxy '*' -sS --connect-timeout 1 --max-time 5 -o "$response" -w '%{http_code}' \
        -H 'Content-Type: application/json' \
        --data-binary "$body" \
        "http://127.0.0.1:$signaling_port/api/publish/allocations")"
    if [[ "$status" != "201" ]]; then
        echo "POST /api/publish/allocations returned $status" >&2
        cat "$response" >&2 2>/dev/null || true
        return 1
    fi
    python3 - "$response" "$protocol" <<'PY'
import json
import sys
import urllib.parse
import uuid

with open(sys.argv[1], encoding="utf-8") as source:
    response = json.load(source)
stream_id = response["stream_id"]
publish_url = response["publish_url"]
parsed_id = uuid.UUID(stream_id)
parsed_url = urllib.parse.urlsplit(publish_url)
query = urllib.parse.parse_qs(parsed_url.query, strict_parsing=True)
assert parsed_id.version == 4 and str(parsed_id) == stream_id
assert parsed_url.scheme == sys.argv[2] and query.get("stream_id") == [stream_id]
print(publish_url)
PY
}

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

wait_log_count() {
    local file="$1"
    local text="$2"
    local expected="$3"
    for _ in $(seq 1 80); do
        if [[ "$(grep -Fc "$text" "$file" 2>/dev/null || true)" -ge "$expected" ]]; then
            return 0
        fi
        sleep 0.1
    done
    echo "missing log count $expected: $text" >&2
    cat "$file" >&2 || true
    return 1
}

allocation_stream_id() {
    local label="$1"
    python3 - "$work_dir/${label}_allocation.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    print(json.load(source)["stream_id"])
PY
}

create_rtsp_source() {
    local label="$1"
    local signaling_port="$2"
    local stream_name="$3"
    local url="$4"
    local request="$work_dir/${label}_source_create_request.json"
    local response="$work_dir/${label}_source_create.json"
    local status

    python3 - "$request" "$stream_name" "$url" <<'PY'
import json
import sys

with open(sys.argv[1], "w", encoding="utf-8") as output:
    json.dump({"stream_name": sys.argv[2], "url": sys.argv[3]}, output)
PY
    status="$(curl --noproxy '*' -sS --connect-timeout 1 --max-time 5 -o "$response" -w '%{http_code}' \
        -H 'Content-Type: application/json' \
        --data-binary "@$request" \
        "http://127.0.0.1:$signaling_port/api/sources")"
    if [[ "$status" != "201" ]]; then
        echo "POST /api/sources returned $status" >&2
        cat "$response" >&2 2>/dev/null || true
        return 1
    fi
    python3 - "$response" "$stream_name" "$url" <<'PY'
import json
import sys
import uuid

with open(sys.argv[1], encoding="utf-8") as source:
    response = json.load(source)
source_id = response["source_id"]
parsed_id = uuid.UUID(source_id)
assert parsed_id.version == 4 and str(parsed_id) == source_id
assert response["stream_name"] == sys.argv[2]
assert response["url"] == sys.argv[3]
assert response["desired_state"] == "stopped"
assert "password" not in response
print(source_id)
PY
}

start_rtsp_source() {
    local label="$1"
    local signaling_port="$2"
    local source_id="$3"
    local response="$work_dir/${label}_source_start.json"
    local status
    status="$(curl --noproxy '*' -sS --connect-timeout 1 --max-time 5 -o "$response" -w '%{http_code}' \
        -X POST "http://127.0.0.1:$signaling_port/api/sources/$source_id/start")"
    if [[ "$status" != "201" ]]; then
        echo "POST source start returned $status" >&2
        cat "$response" >&2 2>/dev/null || true
        return 1
    fi
    python3 - "$response" <<'PY'
import json
import sys
import uuid

with open(sys.argv[1], encoding="utf-8") as source:
    response = json.load(source)
stream_id = response["stream_id"]
parsed_id = uuid.UUID(stream_id)
assert set(response) == {"stream_id"}
assert parsed_id.version == 4 and str(parsed_id) == stream_id
print(stream_id)
PY
}

source_action() {
    local label="$1"
    local signaling_port="$2"
    local source_id="$3"
    local action="$4"
    local method="$5"
    local path_suffix="${6-/$action}"
    local response="$work_dir/${label}_source_${action}.json"
    local status
    status="$(curl --noproxy '*' -sS --connect-timeout 1 --max-time 5 -o "$response" -w '%{http_code}' \
        -X "$method" "http://127.0.0.1:$signaling_port/api/sources/$source_id$path_suffix")"
    if [[ "$status" != "204" ]]; then
        echo "$method source $action returned $status" >&2
        cat "$response" >&2 2>/dev/null || true
        return 1
    fi
    [[ ! -s "$response" ]]
}

stop_rtsp_source() {
    source_action "$1" "$2" "$3" stop POST
}

delete_rtsp_source() {
    source_action "$1" "$2" "$3" delete DELETE ""
}

wait_runtime_state() {
    local signaling_port="$1"
    local stream_id="$2"
    local kind="$3"
    local protocol="$4"
    local stream_name="$5"
    local state="$6"
    local source_id="${7:-}"
    local end_reason="${8:-}"
    local response="$work_dir/runtime_${stream_id}_${state}.json"
    for _ in $(seq 1 100); do
        if curl --noproxy '*' -fsS --connect-timeout 1 --max-time 2 \
            "http://127.0.0.1:$signaling_port/api/runtimes" >"$response" 2>/dev/null && \
            python3 - "$response" "$stream_id" "$kind" "$protocol" "$stream_name" "$state" "$source_id" "$end_reason" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    runtimes = json.load(source)["runtimes"]
for runtime in runtimes:
    if runtime.get("stream_id") != sys.argv[2]:
        continue
    if (runtime.get("kind") == sys.argv[3] and runtime.get("protocol") == sys.argv[4]
            and runtime.get("stream_name") == sys.argv[5] and runtime.get("state") == sys.argv[6]
            and runtime.get("source_id", "") == sys.argv[7]
            and (not sys.argv[8] or runtime.get("end_reason") == sys.argv[8])):
        raise SystemExit(0)
raise SystemExit(1)
PY
        then
            return 0
        fi
        sleep 0.1
    done
    echo "runtime did not reach $state: $stream_id" >&2
    cat "$response" >&2 2>/dev/null || true
    return 1
}

wait_http_stream_absent() {
    local port="$1"
    local stream_name="$2"
    local status
    for _ in $(seq 1 80); do
        status="$(curl -sS --max-time 1 -o /dev/null -w '%{http_code}' \
            "http://127.0.0.1:$port/$stream_name.flv" 2>/dev/null || true)"
        if [[ "$status" == "404" ]]; then
            return 0
        fi
        sleep 0.1
    done
    echo "HTTP stream remained available after RTSP pull delete: $stream_name" >&2
    return 1
}

probe_streams_expected() {
    local output="$1"
    local video_codec="$2"
    local audio_codec="$3"
    shift 3
    timeout 12s ffprobe -v error "$@" \
        -show_entries stream=index,codec_name,codec_type,sample_rate,channels \
        -of compact=p=0:nk=0 >"$output"
    grep -q "codec_name=$video_codec" "$output"
    if [[ -n "$audio_codec" ]]; then
        grep -q "codec_name=$audio_codec" "$output"
    fi
}

probe_streams() {
    local output="$1"
    shift
    probe_streams_expected "$output" h264 aac "$@"
}

wait_probe_streams() {
    local output="$1"
    local video_codec="$2"
    local audio_codec="$3"
    shift 3
    for _ in $(seq 1 50); do
        if probe_streams_expected "$output" "$video_codec" "$audio_codec" "$@" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
    done
    echo "stream probe failed: $*" >&2
    cat "$output" >&2 2>/dev/null || true
    return 1
}

probe_hls_ts() {
    local prefix="$1"
    local url="$2"
    curl -fsS "$url/index.m3u8" >"$work_dir/${prefix}_initial.m3u8"
    sleep 4
    curl -fsS "$url/index.m3u8" >"$work_dir/${prefix}_ready.m3u8"
    local segment_name
    segment_name="$(grep -E '^[^#].*\.ts$' "$work_dir/${prefix}_ready.m3u8" | head -1 | sed 's#^\./##')"
    [[ -n "$segment_name" ]]
    curl -fsS "$url/$segment_name" >"$work_dir/${prefix}_segment.ts"
    [[ $(( $(stat -c%s "$work_dir/${prefix}_segment.ts") % 188 )) -eq 0 ]]
    probe_streams "$work_dir/${prefix}_streams.txt" "$work_dir/${prefix}_segment.ts"
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
    --sip-listen "127.0.0.1:$main_signaling_sip_port" \
    --sip-advertise "127.0.0.1:$main_signaling_sip_port" \
    --http-listen "127.0.0.1:$main_signaling_http_port" \
    --database "$main_signaling_database" \
    >"$work_dir/main_signaling.log" 2>&1 &
main_signaling_pid=$!
wait_http "http://127.0.0.1:$main_signaling_http_port/" "$main_signaling_pid" "$work_dir/main_signaling.log"
kill -0 "$main_signaling_pid"

"$server_bin" --rtmp-port 19350 --rtsp-port 18554 --http-port 18080 \
    --signaling-url "http://127.0.0.1:$main_signaling_http_port" \
    --server-id network-main \
    --control-url 'http://127.0.0.1:18080' \
    --media-ip 127.0.0.1 \
    >"$work_dir/server.log" 2>&1 &
main_pid=$!
wait_http 'http://127.0.0.1:18080/' "$main_pid" "$work_dir/server.log"
kill -0 "$main_pid"

main_publish_url="$(allocate_publish main "$main_signaling_http_port" rtmp live/test)"
main_publish_stream_id="$(allocation_stream_id main)"

ffmpeg -nostdin -hide_banner -loglevel error -re \
    -f lavfi -i 'testsrc=size=320x180:rate=25' \
    -f lavfi -i 'sine=frequency=1000:sample_rate=44100' \
    -map 0:v:0 -map 1:a:0 \
    -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p \
    -g 25 -keyint_min 25 -sc_threshold 0 \
    -c:a aac -b:a 96k -ac 2 \
    -f flv "$main_publish_url" \
    >"$work_dir/publisher.log" 2>&1 &
publish_pid=$!

wait_log "$work_dir/server.log" 'rtmp publish live/test'
wait_log "$work_dir/server.log" 'rtmp publish tracks ready audio true'
wait_runtime_state "$main_signaling_http_port" "$main_publish_stream_id" publisher rtmp live/test streaming
sleep 1

probe_streams "$work_dir/rtsp_from_rtmp.txt" -rtsp_transport tcp 'rtsp://127.0.0.1:18554/live/test'
probe_streams "$work_dir/rtmp_from_rtmp.txt" 'rtmp://127.0.0.1:19350/live/test'
probe_streams "$work_dir/http_flv_from_rtmp.txt" 'http://127.0.0.1:18080/live/test.flv'

# 首次请求建立共享 HLS 输出；等待自然关键帧完成切片。
probe_hls_ts hls_from_rtmp 'http://127.0.0.1:18080/play/hls/live/test'

"$signaling_bin" \
    --sip-listen "127.0.0.1:$pull_signaling_sip_port" \
    --sip-advertise "127.0.0.1:$pull_signaling_sip_port" \
    --http-listen "127.0.0.1:$pull_signaling_http_port" \
    --database "$pull_signaling_database" \
    >"$work_dir/pull_signaling.log" 2>&1 &
pull_signaling_pid=$!
wait_http "http://127.0.0.1:$pull_signaling_http_port/" "$pull_signaling_pid" "$work_dir/pull_signaling.log"
kill -0 "$pull_signaling_pid"

"$server_bin" --rtmp-port 19351 --rtsp-port 18555 --http-port 18081 \
    --signaling-url "http://127.0.0.1:$pull_signaling_http_port" \
    --server-id network-pull \
    --control-url 'http://127.0.0.1:18081' \
    --media-ip 127.0.0.1 \
    >"$work_dir/pull_server.log" 2>&1 &
pull_pid=$!
wait_http 'http://127.0.0.1:18081/' "$pull_pid" "$work_dir/pull_server.log"
kill -0 "$pull_pid"

pull_source_id="$(create_rtsp_source rtsp_pull_initial "$pull_signaling_http_port" relay/test \
    'rtsp://127.0.0.1:18554/live/test')"
pull_stream_id="$(start_rtsp_source rtsp_pull_initial "$pull_signaling_http_port" "$pull_source_id")"

wait_log "$work_dir/pull_server.log" 'rtsp pull connected stream relay/test'
wait_log "$work_dir/pull_server.log" 'rtsp pull tracks ready audio true'
wait_runtime_state "$pull_signaling_http_port" "$pull_stream_id" source rtsp relay/test streaming "$pull_source_id"
wait_probe_streams "$work_dir/rtsp_pull_initial.txt" h264 aac -rtsp_transport tcp \
    'rtsp://127.0.0.1:18555/relay/test'

stop_rtsp_source rtsp_pull_initial "$pull_signaling_http_port" "$pull_source_id"
wait_runtime_state "$pull_signaling_http_port" "$pull_stream_id" source rtsp relay/test stopped "$pull_source_id" requested
wait_http_stream_absent 18081 relay/test
replacement_stream_id="$(start_rtsp_source rtsp_pull_recreate "$pull_signaling_http_port" "$pull_source_id")"
[[ "$replacement_stream_id" != "$pull_stream_id" ]]
wait_log_count "$work_dir/pull_server.log" 'rtsp pull connected stream relay/test' 2
wait_log_count "$work_dir/pull_server.log" 'rtsp pull tracks ready audio true' 2
wait_runtime_state "$pull_signaling_http_port" "$replacement_stream_id" source rtsp relay/test streaming "$pull_source_id"

probe_streams "$work_dir/rtsp_from_rtsp.txt" -rtsp_transport tcp 'rtsp://127.0.0.1:18555/relay/test'
probe_streams "$work_dir/rtmp_from_rtsp.txt" 'rtmp://127.0.0.1:19351/relay/test'
probe_streams "$work_dir/http_flv_from_rtsp.txt" 'http://127.0.0.1:18081/relay/test.flv'

probe_hls_ts hls_from_rtsp 'http://127.0.0.1:18081/play/hls/relay/test'

stop_rtsp_source rtsp_pull_recreate "$pull_signaling_http_port" "$pull_source_id"
wait_runtime_state "$pull_signaling_http_port" "$replacement_stream_id" source rtsp relay/test stopped "$pull_source_id" requested
wait_http_stream_absent 18081 relay/test
delete_rtsp_source rtsp_pull_recreate "$pull_signaling_http_port" "$pull_source_id"

kill -INT "$publish_pid" 2>/dev/null || true
wait "$publish_pid" 2>/dev/null || true
publish_pid=""
wait_runtime_state "$main_signaling_http_port" "$main_publish_stream_id" publisher rtmp live/test stopped

# RTSP publish TCP/UDP 使用独立 stream；UDP 连续建立两次，覆盖传输资源释放后的再次建链。
for publish_case in tcp udp udp-restart; do
    transport="${publish_case%%-*}"
    stream_name="rtsp-publish-$publish_case"
    rtsp_publish_url="$(allocate_publish "rtsp_publish_$publish_case" "$main_signaling_http_port" rtsp "live/$stream_name")"
    rtsp_publish_stream_id="$(allocation_stream_id "rtsp_publish_$publish_case")"
    ffmpeg -nostdin -hide_banner -loglevel error -re \
        -f lavfi -i 'testsrc=size=320x180:rate=25' \
        -f lavfi -i 'sine=frequency=1200:sample_rate=44100' \
        -map 0:v:0 -map 1:a:0 \
        -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p \
        -g 25 -keyint_min 25 -sc_threshold 0 \
        -c:a aac -b:a 96k -ac 2 \
        -t 24 -rtsp_transport "$transport" -f rtsp "$rtsp_publish_url" \
        >"$work_dir/rtsp_publish_${publish_case}.log" 2>&1 &
    rtsp_publish_pid=$!

    wait_probe_streams "$work_dir/rtsp_publish_${publish_case}_rtsp.txt" h264 aac -rtsp_transport tcp \
        "rtsp://127.0.0.1:18554/live/$stream_name"
    wait_runtime_state "$main_signaling_http_port" "$rtsp_publish_stream_id" publisher rtsp "live/$stream_name" streaming
    probe_streams "$work_dir/rtsp_publish_${publish_case}_rtmp.txt" "rtmp://127.0.0.1:19350/live/$stream_name"
    probe_streams "$work_dir/rtsp_publish_${publish_case}_http_flv.txt" "http://127.0.0.1:18080/live/$stream_name.flv"
    probe_hls_ts "rtsp_publish_${publish_case}_hls" "http://127.0.0.1:18080/play/hls/live/$stream_name"

    kill "$rtsp_publish_pid" 2>/dev/null || true
    wait "$rtsp_publish_pid" 2>/dev/null || true
    rtsp_publish_pid=""
    wait_runtime_state "$main_signaling_http_port" "$rtsp_publish_stream_id" publisher rtsp "live/$stream_name" stopped
    sleep 0.2
    kill -0 "$main_pid"
done

# RTSP pull AV1 回归使用独立 H.264 RTSP 源，避免依赖前面已经结束的 RTMP publisher。
av1_source_publish_url="$(allocate_publish av1_source "$main_signaling_http_port" rtsp live/av1-pull-source)"
av1_source_publish_stream_id="$(allocation_stream_id av1_source)"
ffmpeg -nostdin -hide_banner -loglevel error -re \
    -f lavfi -i 'testsrc=size=320x180:rate=25' \
    -f lavfi -i 'sine=frequency=1300:sample_rate=44100' \
    -map 0:v:0 -map 1:a:0 \
    -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p \
    -g 25 -keyint_min 25 -sc_threshold 0 \
    -c:a aac -b:a 96k -ac 2 \
    -rtsp_transport tcp -f rtsp "$av1_source_publish_url" \
    >"$work_dir/av1_rtsp_pull_source.log" 2>&1 &
rtsp_publish_pid=$!

wait_probe_streams "$work_dir/av1_rtsp_pull_source.txt" h264 aac -rtsp_transport tcp \
    'rtsp://127.0.0.1:18554/live/av1-pull-source'
wait_runtime_state "$main_signaling_http_port" "$av1_source_publish_stream_id" publisher rtsp live/av1-pull-source streaming

# AV1 作为显式输出能力启用：RTMP/HTTP-FLV 使用 Enhanced FLV，HLS 使用 fMP4，RTSP 使用 AV1/RTP。
"$signaling_bin" \
    --sip-listen "127.0.0.1:$av1_signaling_sip_port" \
    --sip-advertise "127.0.0.1:$av1_signaling_sip_port" \
    --http-listen "127.0.0.1:$av1_signaling_http_port" \
    --database "$av1_signaling_database" \
    >"$work_dir/av1_signaling.log" 2>&1 &
av1_signaling_pid=$!
wait_http "http://127.0.0.1:$av1_signaling_http_port/" "$av1_signaling_pid" "$work_dir/av1_signaling.log"
kill -0 "$av1_signaling_pid"

"$server_bin" --rtmp-port 19352 --rtsp-port 18556 --http-port 18082 \
    --rtmp-video-codec av1 --rtsp-video-codec av1 --http-video-codec av1 \
    --signaling-url "http://127.0.0.1:$av1_signaling_http_port" \
    --server-id network-av1 \
    --control-url 'http://127.0.0.1:18082' \
    --media-ip 127.0.0.1 \
    >"$work_dir/av1_server.log" 2>&1 &
av1_server_pid=$!
wait_http 'http://127.0.0.1:18082/' "$av1_server_pid" "$work_dir/av1_server.log"
kill -0 "$av1_server_pid"

av1_pull_source_id="$(create_rtsp_source rtsp_pull_av1 "$av1_signaling_http_port" relay/av1 \
    'rtsp://127.0.0.1:18554/live/av1-pull-source')"
av1_pull_stream_id="$(start_rtsp_source rtsp_pull_av1 "$av1_signaling_http_port" "$av1_pull_source_id")"

av1_publish_url="$(allocate_publish av1 "$av1_signaling_http_port" rtmp live/av1)"
av1_publish_stream_id="$(allocation_stream_id av1)"
ffmpeg -nostdin -hide_banner -loglevel error -re \
    -f lavfi -i 'testsrc=size=320x180:rate=25' \
    -f lavfi -i 'sine=frequency=1400:sample_rate=44100' \
    -map 0:v:0 -map 1:a:0 \
    -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p \
    -g 25 -keyint_min 25 -sc_threshold 0 \
    -c:a aac -b:a 96k -ac 2 \
    -f flv "$av1_publish_url" \
    >"$work_dir/av1_publisher.log" 2>&1 &
av1_publish_pid=$!

wait_log "$work_dir/av1_server.log" 'rtmp publish live/av1'
wait_log "$work_dir/av1_server.log" 'rtsp pull connected stream relay/av1'
wait_log "$work_dir/av1_server.log" 'rtsp pull tracks ready audio true'
wait_runtime_state "$av1_signaling_http_port" "$av1_publish_stream_id" publisher rtmp live/av1 streaming
wait_runtime_state "$av1_signaling_http_port" "$av1_pull_stream_id" source rtsp relay/av1 streaming "$av1_pull_source_id"

wait_probe_streams "$work_dir/rtsp_av1_from_rtmp.txt" av1 aac -rtsp_transport tcp \
    'rtsp://127.0.0.1:18556/live/av1'
wait_probe_streams "$work_dir/rtsp_av1_from_pull.txt" av1 aac -rtsp_transport tcp \
    'rtsp://127.0.0.1:18556/relay/av1'

stop_rtsp_source rtsp_pull_av1 "$av1_signaling_http_port" "$av1_pull_source_id"
wait_runtime_state "$av1_signaling_http_port" "$av1_pull_stream_id" source rtsp relay/av1 stopped "$av1_pull_source_id" requested
wait_http_stream_absent 18082 relay/av1
delete_rtsp_source rtsp_pull_av1 "$av1_signaling_http_port" "$av1_pull_source_id"

# 快速连接/断开多个 AV1 RTSP client，随后确认会话和转码器仍可正常重新建立。
for _ in $(seq 1 3); do
    timeout 2s ffprobe -v error -rtsp_transport tcp \
        -show_entries stream=codec_name \
        -of compact=p=0:nk=0 \
        'rtsp://127.0.0.1:18556/live/av1' >/dev/null 2>&1 || true
done
wait_probe_streams "$work_dir/rtsp_av1_after_churn.txt" av1 aac -rtsp_transport tcp \
    'rtsp://127.0.0.1:18556/live/av1'
kill -0 "$av1_server_pid"

kill "$rtsp_publish_pid" 2>/dev/null || true
wait "$rtsp_publish_pid" 2>/dev/null || true
rtsp_publish_pid=""
wait_runtime_state "$main_signaling_http_port" "$av1_source_publish_stream_id" publisher rtsp live/av1-pull-source stopped

# RTMP AV1 必须由 peer 通过 legacy fourCcList 显式声明 av01；未声明时不能回退到其他视频编码。
wait_probe_streams "$work_dir/rtmp_av1.txt" av1 aac -rtmp_enhanced_codecs av01 'rtmp://127.0.0.1:19352/live/av1'
timeout 3s ffprobe -v error \
    -show_entries stream=codec_name \
    -of compact=p=0:nk=0 \
    'rtmp://127.0.0.1:19352/live/av1' >"$work_dir/rtmp_av1_without_capability.txt" 2>&1 || true
if grep -q 'codec_name=' "$work_dir/rtmp_av1_without_capability.txt"; then
    echo 'rtmp av1 unexpectedly served peer without av01 capability' >&2
    cat "$work_dir/rtmp_av1_without_capability.txt" >&2
    exit 1
fi
wait_probe_streams "$work_dir/http_flv_av1.txt" av1 aac 'http://127.0.0.1:18082/live/av1.flv'
wait_probe_streams "$work_dir/hls_av1.txt" av1 aac 'http://127.0.0.1:18082/play/hls/live/av1/index.m3u8'

curl -fsS 'http://127.0.0.1:18082/play/hls/live/av1/index.m3u8' >"$work_dir/hls_av1.m3u8"
av1_init_uri="$(sed -n 's/^#EXT-X-MAP:URI="\(\.\/init\.mp4?v=[0-9][0-9]*\)"$/\1/p' "$work_dir/hls_av1.m3u8" | head -1)"
[[ -n "$av1_init_uri" ]]
av1_segment="$(grep -E '^[^#].*\.m4s$' "$work_dir/hls_av1.m3u8" | head -1 | sed 's#^\./##')"
[[ -n "$av1_segment" ]]
curl -fsS "http://127.0.0.1:18082/play/hls/live/av1/${av1_init_uri#./}" >"$work_dir/hls_av1_init.mp4"
curl -fsS "http://127.0.0.1:18082/play/hls/live/av1/$av1_segment" >"$work_dir/hls_av1_segment.m4s"
[[ -s "$work_dir/hls_av1_init.mp4" ]]
[[ -s "$work_dir/hls_av1_segment.m4s" ]]

# RTSP publish TCP/UDP 继续以 H.264 输入，验证同一 RTSP 服务的 AV1 play。
for transport in tcp udp; do
    stream_name="rtsp-av1-$transport"
    rtsp_publish_url="$(allocate_publish "rtsp_av1_$transport" "$av1_signaling_http_port" rtsp "live/$stream_name")"
    rtsp_publish_stream_id="$(allocation_stream_id "rtsp_av1_$transport")"
    ffmpeg -nostdin -hide_banner -loglevel error -re \
        -f lavfi -i 'testsrc=size=320x180:rate=25' \
        -f lavfi -i 'sine=frequency=1500:sample_rate=44100' \
        -map 0:v:0 -map 1:a:0 \
        -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p \
        -g 25 -keyint_min 25 -sc_threshold 0 \
        -c:a aac -b:a 96k -ac 2 \
        -rtsp_transport "$transport" -f rtsp "$rtsp_publish_url" \
        >"$work_dir/rtsp_av1_publish_${transport}.log" 2>&1 &
    rtsp_publish_pid=$!

    wait_probe_streams "$work_dir/rtsp_av1_push_${transport}.txt" av1 aac -rtsp_transport tcp \
        "rtsp://127.0.0.1:18556/live/$stream_name"
    wait_runtime_state "$av1_signaling_http_port" "$rtsp_publish_stream_id" publisher rtsp "live/$stream_name" streaming

    kill "$rtsp_publish_pid" 2>/dev/null || true
    wait "$rtsp_publish_pid" 2>/dev/null || true
    rtsp_publish_pid=""
    wait_runtime_state "$av1_signaling_http_port" "$rtsp_publish_stream_id" publisher rtsp "live/$stream_name" stopped
    sleep 0.2
    kill -0 "$av1_server_pid"
done

kill "$av1_publish_pid" 2>/dev/null || true
wait "$av1_publish_pid" 2>/dev/null || true
av1_publish_pid=""
wait_runtime_state "$av1_signaling_http_port" "$av1_publish_stream_id" publisher rtmp live/av1 stopped

kill -0 "$main_pid"
kill -0 "$pull_pid"
kill -0 "$av1_server_pid"

if grep -Fq 'rtsp av1 transcode failed' "$work_dir/av1_server.log"; then
    echo 'rtsp av1 transcode failure detected' >&2
    grep -F 'rtsp av1 transcode failed' "$work_dir/av1_server.log" >&2
    exit 1
fi

cat >"$work_dir/summary.txt" <<SUMMARY
rtmp publish -> rtsp play: pass
rtmp publish -> rtmp play: pass
rtmp publish -> http-flv streamer: pass
rtmp publish -> hls segmenter: pass
rtsp pull -> rtsp play: pass
rtsp pull -> rtmp play: pass
rtsp pull -> http-flv streamer: pass
rtsp pull -> hls segmenter: pass
control plane source stop/recreate generation: pass
rtsp publish tcp -> rtsp play/rtmp play/http-flv streamer/hls segmenter: pass
rtsp publish udp -> rtsp play/rtmp play/http-flv streamer/hls segmenter: pass
rtsp publish udp restart -> rtsp play/rtmp play/http-flv streamer/hls segmenter: pass
rtmp publish -> rtsp av1 play: pass
rtsp pull -> rtsp av1 play: pass
rtsp publish tcp -> rtsp av1 play: pass
rtsp publish udp -> rtsp av1 play: pass
rtsp av1 client churn: pass
rtmp explicit av1 output: pass
rtmp av1 rejects peer without av01: pass
http-flv explicit av1 output: pass
hls explicit av1 fmp4 output: pass
runtime events -> observed state: pass
all servers remained alive after client disconnects: pass
SUMMARY
cat "$work_dir/summary.txt"
