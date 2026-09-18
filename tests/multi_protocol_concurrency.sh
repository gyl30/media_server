#!/usr/bin/env bash
set -euo pipefail

server_bin="${1:-./build/media_server}"
work_dir="${2:-./multi_protocol_concurrency_output}"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
signaling_bin="${SIGNALING_BIN:-}"
ffmpeg_bin="${FFMPEG_BIN:-ffmpeg}"
signaling_port="${MEDIA_SERVER_CONCURRENCY_SIGNALING_PORT:-19110}"
rtmp_port="${MEDIA_SERVER_CONCURRENCY_RTMP_PORT:-19410}"
rtsp_port="${MEDIA_SERVER_CONCURRENCY_RTSP_PORT:-18610}"
http_port="${MEDIA_SERVER_CONCURRENCY_HTTP_PORT:-18110}"
scenario="${3:-concurrency}"
stream_name=""

mkdir -p "$work_dir"
work_dir="$(cd "$work_dir" && pwd)"
server_bin="$(realpath "$server_bin")"
database="$(mktemp "$work_dir/signaling.XXXXXX.db")"

signaling_pid=""
server_pid=""
publisher_pid=""
declare -a player_pids=()

cleanup() {
    set +e
    for pid in "${player_pids[@]}"; do
        kill -TERM "$pid" 2>/dev/null
    done
    [[ -n "$publisher_pid" ]] && kill -TERM "$publisher_pid" 2>/dev/null
    [[ -n "$server_pid" ]] && kill -TERM "$server_pid" 2>/dev/null
    [[ -n "$signaling_pid" ]] && kill -TERM "$signaling_pid" 2>/dev/null
    for pid in "${player_pids[@]}"; do
        wait "$pid" 2>/dev/null
    done
    [[ -n "$publisher_pid" ]] && wait "$publisher_pid" 2>/dev/null
    [[ -n "$server_pid" ]] && wait "$server_pid" 2>/dev/null
    [[ -n "$signaling_pid" ]] && wait "$signaling_pid" 2>/dev/null
}
trap cleanup EXIT

wait_http() {
    local url="$1"
    local pid="$2"
    local log="$3"
    for _ in $(seq 1 100); do
        if curl --noproxy '*' -sS --max-time 1 -o /dev/null "$url" 2>/dev/null; then
            kill -0 "$pid" 2>/dev/null
            return
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

allocate() {
    local operation="$1"
    local label="$2"
    local protocol="$3"
    local response="$work_dir/${label}_allocation.json"
    local body
    local status
    printf -v body '{"protocol":"%s","stream_name":"%s"}' "$protocol" "$stream_name"
    status="$(curl --noproxy '*' -sS --connect-timeout 1 --max-time 5 -o "$response" -w '%{http_code}' \
        -H 'Content-Type: application/json' --data-binary "$body" \
        "http://127.0.0.1:$signaling_port/api/$operation/allocations")"
    if [[ "$status" != "201" ]]; then
        echo "$operation allocation for $label returned $status" >&2
        cat "$response" >&2 2>/dev/null || true
        return 1
    fi
    python3 - "$response" "${operation}_url" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    print(json.load(source)[sys.argv[2]])
PY
}

allocation_stream_id() {
    python3 - "$work_dir/${1}_allocation.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    print(json.load(source)["stream_id"])
PY
}

wait_runtime_state() {
    local stream_id="$1"
    local kind="$2"
    local protocol="$3"
    local state="$4"
    local attempts="${5:-150}"
    local response="$work_dir/runtime_${stream_id}_${state}.json"
    for _ in $(seq 1 "$attempts"); do
        if curl --noproxy '*' -fsS --connect-timeout 1 --max-time 2 \
            "http://127.0.0.1:$signaling_port/api/runtimes" >"$response" 2>/dev/null && \
            python3 - "$response" "$stream_id" "$kind" "$protocol" "$stream_name" "$state" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    runtimes = json.load(source)["runtimes"]
for runtime in runtimes:
    if (runtime.get("stream_id") == sys.argv[2]
            and runtime.get("kind") == sys.argv[3]
            and runtime.get("protocol") == sys.argv[4]
            and runtime.get("stream_name") == sys.argv[5]
            and runtime.get("state") == sys.argv[6]):
        raise SystemExit(0)
raise SystemExit(1)
PY
        then
            return
        fi
        sleep 0.1
    done
    echo "$protocol runtime $stream_id did not reach $state for $stream_name" >&2
    cat "$response" >&2 2>/dev/null || true
    return 1
}

establish_hls_session() {
    local label="$1"
    local initial_url="$2"
    local headers="$work_dir/${label}_redirect_headers.txt"
    local status
    local location
    status="$(curl --noproxy '*' -sS --connect-timeout 1 --max-time 5 --max-redirs 0 \
        -D "$headers" -o /dev/null -w '%{http_code}' "$initial_url")"
    if [[ "$status" != "307" ]]; then
        echo "HLS admission for $label returned $status" >&2
        cat "$headers" >&2 2>/dev/null || true
        return 1
    fi
    location="$(sed -n 's/^[Ll]ocation:[[:space:]]*//p' "$headers" | tr -d '\r' | tail -1)"
    python3 - "$initial_url" "$location" <<'PY'
import sys
import urllib.parse

assert "stream_id=" not in sys.argv[2] and "session=" in sys.argv[2]
print(urllib.parse.urljoin(sys.argv[1], sys.argv[2]))
PY
}

start_player() {
    local label="$1"
    local protocol="$2"
    local url="$3"
    local -a input_options=()
    if [[ "$protocol" == "rtsp" ]]; then
        input_options=(-rtsp_transport tcp)
    fi
    timeout 20s "$ffmpeg_bin" -nostdin -hide_banner -loglevel error \
        "${input_options[@]}" -i "$url" -t 12 -map 0:v:0 -f null - \
        >"$work_dir/${label}.log" 2>&1 &
    player_pids+=("$!")
}

start_publisher() {
    local label="$1"
    local video_source="${2:-testsrc=size=320x180:rate=25}"
    local audio_frequency="${3:-1000}"
    local publish_url
    publish_url="$(allocate publish "$label" rtmp)"
    publisher_stream_id="$(allocation_stream_id "$label")"
    "$ffmpeg_bin" -nostdin -hide_banner -loglevel error -re \
        -f lavfi -i "$video_source" \
        -f lavfi -i "sine=frequency=$audio_frequency:sample_rate=44100" \
        -map 0:v:0 -map 1:a:0 -c:v libx264 -preset ultrafast -tune zerolatency \
        -pix_fmt yuv420p -g 25 -keyint_min 25 -sc_threshold 0 -c:a aac -b:a 96k -ac 2 \
        -f flv "$publish_url" >"$work_dir/${label}.log" 2>&1 &
    publisher_pid=$!
    wait_runtime_state "$publisher_stream_id" publisher rtmp streaming
}

stop_publisher() {
    kill -INT "$publisher_pid"
    wait "$publisher_pid" 2>/dev/null || true
    publisher_pid=""
    wait_runtime_state "$publisher_stream_id" publisher rtmp stopped
}

probe_once() {
    local label="$1"
    local protocol="$2"
    local url="$3"
    local -a input_options=()
    if [[ "$protocol" == "rtsp" ]]; then
        input_options=(-rtsp_transport tcp)
    fi
    if ! timeout 15s "$ffmpeg_bin" -nostdin -hide_banner -loglevel error \
        "${input_options[@]}" -i "$url" -t 1 -map 0:v:0 -f null - \
        >"$work_dir/${label}.log" 2>&1; then
        echo "$protocol churn player failed for $stream_name" >&2
        cat "$work_dir/${label}.log" >&2 2>/dev/null || true
        return 1
    fi
}

probe_hls_session() {
    local label="$1"
    local index_url="$2"
    local playlist="$work_dir/${label}.m3u8"
    local segment_uri
    local segment_url
    curl --noproxy '*' -fsS --connect-timeout 1 --max-time 12 "$index_url" >"$playlist"
    segment_uri="$(grep -E '^[^#].*\.(ts|m4s)(\?.*)?$' "$playlist" | head -1)"
    [[ -n "$segment_uri" && "$segment_uri" == *session=* ]]
    segment_url="$(python3 - "$index_url" "$segment_uri" <<'PY'
import sys
import urllib.parse

print(urllib.parse.urljoin(sys.argv[1], sys.argv[2]))
PY
)"
    curl --noproxy '*' -fsS --connect-timeout 1 --max-time 5 "$segment_url" >"$work_dir/${label}.segment"
    [[ -s "$work_dir/${label}.segment" ]]
}

wait_player_exit() {
    local pid="$1"
    local label="$2"
    for _ in $(seq 1 100); do
        if ! kill -0 "$pid" 2>/dev/null; then
            wait "$pid" 2>/dev/null || true
            return
        fi
        sleep 0.1
    done
    echo "$label did not terminate after source end" >&2
    cat "$work_dir/${label}.log" >&2 2>/dev/null || true
    return 1
}

wait_hls_endlist() {
    local label="$1"
    local index_url="$2"
    local playlist="$work_dir/${label}.m3u8"
    for _ in $(seq 1 100); do
        if curl --noproxy '*' -fsS --connect-timeout 1 --max-time 2 "$index_url" >"$playlist" 2>/dev/null && \
            grep -Fq '#EXT-X-ENDLIST' "$playlist"; then
            return
        fi
        sleep 0.1
    done
    echo "HLS viewer did not observe source end: $label" >&2
    cat "$playlist" >&2 2>/dev/null || true
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

"$signaling_bin" --sip-listen 127.0.0.1:15110 --sip-advertise 127.0.0.1:15110 \
    --http-listen "127.0.0.1:$signaling_port" --database "$database" \
    >"$work_dir/signaling.log" 2>&1 &
signaling_pid=$!
wait_http "http://127.0.0.1:$signaling_port/" "$signaling_pid" "$work_dir/signaling.log"

"$server_bin" --rtmp-port "$rtmp_port" --rtsp-port "$rtsp_port" --http-port "$http_port" \
    --signaling-url "http://127.0.0.1:$signaling_port" --server-id concurrency \
    --control-url "http://127.0.0.1:$http_port" --media-ip 127.0.0.1 \
    >"$work_dir/server.log" 2>&1 &
server_pid=$!
wait_http "http://127.0.0.1:$http_port/" "$server_pid" "$work_dir/server.log"

run_concurrency() {
    stream_name=live/concurrency
    start_publisher publisher

    local protocol
    local index
    local label
    local play_url
    local stream_id
    local -a viewer_labels=()
    local -a viewer_protocols=()
    local -a viewer_ids=()
    local -a hls_urls=()
    for protocol in rtsp rtmp http-flv hls; do
        for index in 1 2; do
            label="${protocol//-/_}_${index}"
            play_url="$(allocate play "$label" "$protocol")"
            stream_id="$(allocation_stream_id "$label")"
            viewer_labels+=("$label")
            viewer_protocols+=("$protocol")
            viewer_ids+=("$stream_id")
            if [[ "$protocol" == "hls" ]]; then
                play_url="$(establish_hls_session "$label" "$play_url")"
                hls_urls+=("$play_url")
            fi
            start_player "$label" "$protocol" "$play_url"
        done
    done

    python3 - "$stream_name" "$work_dir" "${viewer_labels[@]}" <<'PY'
import json
import pathlib
import sys
import urllib.parse
import uuid

stream_name = sys.argv[1]
work_dir = pathlib.Path(sys.argv[2])
identities = []
for label in sys.argv[3:]:
    with (work_dir / f"{label}_allocation.json").open(encoding="utf-8") as source:
        allocation = json.load(source)
    stream_id = allocation["stream_id"]
    parsed = uuid.UUID(stream_id)
    url = urllib.parse.urlsplit(allocation["play_url"])
    assert parsed.version == 4 and str(parsed) == stream_id
    assert urllib.parse.parse_qs(url.query, strict_parsing=True).get("stream_id") == [stream_id]
    if label.startswith("rtsp_"):
        assert url.scheme == "rtsp" and url.path == f"/{stream_name}"
    elif label.startswith("rtmp_"):
        assert url.scheme == "rtmp" and url.path == f"/{stream_name}"
    elif label.startswith("http_flv_"):
        assert url.scheme == "http" and url.path == f"/{stream_name}.flv"
    else:
        assert label.startswith("hls_")
        assert url.scheme == "http" and url.path == f"/play/hls/{stream_name}/index.m3u8"
    identities.append(stream_id)
assert len(identities) == len(set(identities))
PY

    python3 - "${hls_urls[@]}" <<'PY'
import sys
import urllib.parse

secrets = [urllib.parse.parse_qs(urllib.parse.urlsplit(url).query)["session"][0] for url in sys.argv[1:]]
assert len(secrets) == 2 and len(set(secrets)) == 2
PY

    for index in "${!viewer_ids[@]}"; do
        wait_runtime_state "${viewer_ids[$index]}" output "${viewer_protocols[$index]}" streaming
    done

    for index in "${!player_pids[@]}"; do
        if ! wait "${player_pids[$index]}"; then
            echo "${viewer_protocols[$index]} player ${viewer_ids[$index]} failed for $stream_name" >&2
            cat "$work_dir/${viewer_labels[$index]}.log" >&2 2>/dev/null || true
            return 1
        fi
    done
    player_pids=()
    stop_publisher
    echo "multi-protocol concurrency passed"
}

run_churn() {
    stream_name=live/churn
    start_publisher churn_publisher

    local rounds="${MEDIA_SERVER_STRESS_ROUNDS:-10}"
    local protocol
    local round
    local label
    local play_url
    local stream_id
    local status
    local -a viewer_labels=()
    local -a viewer_ids=()
    local -a viewer_protocols=()
    local -a hls_urls=()
    for protocol in rtsp rtmp http-flv; do
        for round in $(seq 1 "$rounds"); do
            label="churn_${protocol//-/_}_$round"
            play_url="$(allocate play "$label" "$protocol")"
            stream_id="$(allocation_stream_id "$label")"
            viewer_labels+=("$label")
            viewer_ids+=("$stream_id")
            viewer_protocols+=("$protocol")
            probe_once "$label" "$protocol" "$play_url"
            wait_runtime_state "$stream_id" output "$protocol" stopped
        done
    done

    for round in $(seq 1 "$rounds"); do
        label="churn_hls_$round"
        play_url="$(allocate play "$label" hls)"
        stream_id="$(allocation_stream_id "$label")"
        play_url="$(establish_hls_session "$label" "$play_url")"
        viewer_labels+=("$label")
        viewer_ids+=("$stream_id")
        viewer_protocols+=(hls)
        hls_urls+=("$play_url")
        probe_hls_session "$label" "$play_url"
        wait_runtime_state "$stream_id" output hls streaming
    done

    python3 - "$work_dir" "$rounds" "${viewer_labels[@]}" <<'PY'
import json
import pathlib
import sys
import uuid

work_dir = pathlib.Path(sys.argv[1])
rounds = int(sys.argv[2])
identities = []
for label in sys.argv[3:]:
    with (work_dir / f"{label}_allocation.json").open(encoding="utf-8") as source:
        stream_id = json.load(source)["stream_id"]
    parsed = uuid.UUID(stream_id)
    assert parsed.version == 4 and str(parsed) == stream_id
    identities.append(stream_id)
assert len(identities) == rounds * 4
assert len(identities) == len(set(identities))
PY

    python3 - "${hls_urls[@]}" <<'PY'
import sys
import urllib.parse

secrets = [urllib.parse.parse_qs(urllib.parse.urlsplit(url).query)["session"][0] for url in sys.argv[1:]]
assert len(secrets) == len(set(secrets))
PY

    for index in "${!viewer_ids[@]}"; do
        if [[ "${viewer_protocols[$index]}" == "hls" ]]; then
            wait_runtime_state "${viewer_ids[$index]}" output hls stopped 350
        fi
    done
    for index in "${!hls_urls[@]}"; do
        status="$(curl --noproxy '*' -sS --connect-timeout 1 --max-time 5 -o /dev/null -w '%{http_code}' "${hls_urls[$index]}")"
        if [[ "$status" != "403" ]]; then
            echo "expired HLS viewer ${viewer_ids[$((rounds * 3 + index))]} returned $status" >&2
            return 1
        fi
    done

    stop_publisher
    echo "multi-protocol churn passed: $rounds rounds per protocol"
}

run_replacement() {
    stream_name=live/replacement
    start_publisher replacement_publisher_a
    local first_publisher_id="$publisher_stream_id"
    local protocol
    local label
    local play_url
    local stream_id
    local index
    local -a first_labels=()
    local -a first_protocols=()
    local -a first_ids=()

    for protocol in rtsp rtmp http-flv; do
        label="replacement_a_${protocol//-/_}"
        play_url="$(allocate play "$label" "$protocol")"
        stream_id="$(allocation_stream_id "$label")"
        first_labels+=("$label")
        first_protocols+=("$protocol")
        first_ids+=("$stream_id")
        start_player "$label" "$protocol" "$play_url"
    done
    label=replacement_a_hls
    play_url="$(allocate play "$label" hls)"
    stream_id="$(allocation_stream_id "$label")"
    play_url="$(establish_hls_session "$label" "$play_url")"
    first_labels+=("$label")
    first_protocols+=(hls)
    first_ids+=("$stream_id")
    local first_hls_url="$play_url"
    probe_hls_session "$label" "$first_hls_url"

    for index in "${!first_ids[@]}"; do
        wait_runtime_state "${first_ids[$index]}" output "${first_protocols[$index]}" streaming
    done

    stop_publisher
    for index in "${!player_pids[@]}"; do
        wait_player_exit "${player_pids[$index]}" "${first_labels[$index]}"
        wait_runtime_state "${first_ids[$index]}" output "${first_protocols[$index]}" stopped
    done
    player_pids=()
    wait_hls_endlist replacement_a_ended "$first_hls_url"

    start_publisher replacement_publisher_b 'testsrc2=size=320x180:rate=25' 1200
    local second_publisher_id="$publisher_stream_id"
    [[ "$second_publisher_id" != "$first_publisher_id" ]]

    local -a second_labels=()
    local -a second_protocols=()
    local -a second_ids=()
    for protocol in rtsp rtmp http-flv; do
        label="replacement_b_${protocol//-/_}"
        play_url="$(allocate play "$label" "$protocol")"
        stream_id="$(allocation_stream_id "$label")"
        second_labels+=("$label")
        second_protocols+=("$protocol")
        second_ids+=("$stream_id")
        probe_once "$label" "$protocol" "$play_url"
        wait_runtime_state "$stream_id" output "$protocol" stopped
    done
    label=replacement_b_hls
    play_url="$(allocate play "$label" hls)"
    stream_id="$(allocation_stream_id "$label")"
    play_url="$(establish_hls_session "$label" "$play_url")"
    second_labels+=("$label")
    second_protocols+=(hls)
    second_ids+=("$stream_id")
    local second_hls_url="$play_url"
    [[ "$second_hls_url" != "$first_hls_url" ]]
    probe_hls_session "$label" "$second_hls_url"
    wait_runtime_state "$stream_id" output hls streaming
    if grep -Fq '#EXT-X-ENDLIST' "$work_dir/${label}.m3u8"; then
        echo "replacement HLS viewer received ended playlist" >&2
        return 1
    fi

    wait_hls_endlist replacement_a_retained "$first_hls_url"
    probe_hls_session replacement_a_retained "$first_hls_url"
    cmp "$work_dir/replacement_a_hls.segment" "$work_dir/replacement_a_retained.segment"
    if cmp -s "$work_dir/replacement_a_hls.segment" "$work_dir/replacement_b_hls.segment"; then
        echo "replacement HLS segment reused old source data" >&2
        return 1
    fi

    python3 - "$work_dir" "$first_publisher_id" "$second_publisher_id" "${first_labels[@]}" "${second_labels[@]}" <<'PY'
import json
import pathlib
import sys

work_dir = pathlib.Path(sys.argv[1])
identities = [sys.argv[2], sys.argv[3]]
for label in sys.argv[4:]:
    with (work_dir / f"{label}_allocation.json").open(encoding="utf-8") as source:
        identities.append(json.load(source)["stream_id"])
assert len(identities) == len(set(identities))
PY

    stop_publisher
    echo "multi-protocol source replacement passed"
}

case "$scenario" in
    concurrency)
        run_concurrency
        ;;
    churn)
        run_churn
        ;;
    replacement)
        run_replacement
        ;;
    *)
        echo "unknown scenario: $scenario" >&2
        exit 2
        ;;
esac
