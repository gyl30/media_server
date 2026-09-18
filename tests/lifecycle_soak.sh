#!/usr/bin/env bash
set -euo pipefail

server_bin="${1:-./build/media_server}"
work_dir="${2:-./lifecycle_soak_output}"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
signaling_bin="${SIGNALING_BIN:-}"
soak_rounds="${MEDIA_SERVER_SOAK_ROUNDS:-20}"
replacement_rounds="${MEDIA_SERVER_REPLACEMENT_ROUNDS:-20}"
signaling_port="${MEDIA_SERVER_SOAK_SIGNALING_PORT:-19210}"
rtmp_port="${MEDIA_SERVER_SOAK_RTMP_PORT:-19510}"
rtsp_port="${MEDIA_SERVER_SOAK_RTSP_PORT:-18710}"
http_port="${MEDIA_SERVER_SOAK_HTTP_PORT:-18210}"
ffmpeg_bin="${FFMPEG_BIN:-ffmpeg}"

[[ "$soak_rounds" =~ ^[1-9][0-9]*$ ]]
[[ "$replacement_rounds" =~ ^[1-9][0-9]*$ ]]
mkdir -p "$work_dir"
work_dir="$(cd "$work_dir" && pwd)"
server_bin="$(realpath "$server_bin")"
database="$(mktemp "$work_dir/signaling.XXXXXX.db")"
ids_file="$work_dir/stream_ids.txt"
resources_file="$work_dir/resources.tsv"
: >"$ids_file"
printf 'round\tfd\trss_kb\tvmsize_kb\tthreads\n' >"$resources_file"

signaling_pid=""
server_pid=""
publisher_pid=""
publisher_stream_id=""
declare -a player_pids=()
declare -a hls_ids=()
declare -a hls_urls=()

cleanup() {
    set +e
    for pid in "${player_pids[@]}"; do kill -TERM "$pid" 2>/dev/null; done
    [[ -n "$publisher_pid" ]] && kill -TERM "$publisher_pid" 2>/dev/null
    [[ -n "$server_pid" ]] && kill -TERM "$server_pid" 2>/dev/null
    [[ -n "$signaling_pid" ]] && kill -TERM "$signaling_pid" 2>/dev/null
    for pid in "${player_pids[@]}"; do wait "$pid" 2>/dev/null; done
    [[ -n "$publisher_pid" ]] && wait "$publisher_pid" 2>/dev/null
    [[ -n "$server_pid" ]] && wait "$server_pid" 2>/dev/null
    [[ -n "$signaling_pid" ]] && wait "$signaling_pid" 2>/dev/null
}
trap cleanup EXIT

wait_http() {
    local url="$1" pid="$2" log="$3"
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
    echo "endpoint did not become ready: $url" >&2
    cat "$log" >&2 2>/dev/null || true
    return 1
}

sample_resources() {
    local label="$1" fd rss vmsize threads
    fd="$(find "/proc/$server_pid/fd" -mindepth 1 -maxdepth 1 -type l 2>/dev/null | wc -l)"
    rss="$(awk '/^VmRSS:/{print $2}' "/proc/$server_pid/status")"
    vmsize="$(awk '/^VmSize:/{print $2}' "/proc/$server_pid/status")"
    threads="$(awk '/^Threads:/{print $2}' "/proc/$server_pid/status")"
    printf '%s\t%s\t%s\t%s\t%s\n' "$label" "$fd" "$rss" "$vmsize" "$threads" | tee -a "$resources_file"
}

record_id() {
    local id="$1"
    printf '%s\n' "$id" >>"$ids_file"
}

allocate() {
    local operation="$1" label="$2" protocol="$3" stream_name="$4" response body status
    response="$work_dir/${label}_allocation.json"
    printf -v body '{"protocol":"%s","stream_name":"%s"}' "$protocol" "$stream_name"
    status="$(curl --noproxy '*' -sS --connect-timeout 1 --max-time 5 -o "$response" -w '%{http_code}' \
        -H 'Content-Type: application/json' --data-binary "$body" \
        "http://127.0.0.1:$signaling_port/api/$operation/allocations")"
    if [[ "$status" != 201 ]]; then
        echo "$operation allocation $label returned $status" >&2
        cat "$response" >&2 2>/dev/null || true
        return 1
    fi
    python3 - "$response" "${operation}_url" <<'PY'
import json
import sys
import uuid

with open(sys.argv[1], encoding="utf-8") as source:
    response = json.load(source)
stream_id = response["stream_id"]
parsed = uuid.UUID(stream_id)
assert parsed.version == 4 and str(parsed) == stream_id
print(stream_id)
print(response[sys.argv[2]])
PY
}

allocate_values() {
    local operation="$1" label="$2" protocol="$3" stream_name="$4" values
    values="$(allocate "$operation" "$label" "$protocol" "$stream_name")"
    mapfile -t allocation_values <<<"$values"
    allocation_id="${allocation_values[0]}"
    allocation_url="${allocation_values[1]}"
    record_id "$allocation_id"
}

wait_runtime() {
    local stream_id="$1" kind="$2" protocol="$3" stream_name="$4" state="$5" attempts="${6:-150}"
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
    if all(runtime.get(key) == value for key, value in {
        "stream_id": sys.argv[2], "kind": sys.argv[3], "protocol": sys.argv[4],
        "stream_name": sys.argv[5], "state": sys.argv[6]}.items()):
        raise SystemExit(0)
raise SystemExit(1)
PY
        then
            return
        fi
        sleep 0.1
    done
    echo "runtime did not reach $state: $protocol $stream_id $stream_name" >&2
    cat "$response" >&2 2>/dev/null || true
    return 1
}

start_publisher() {
    local label="$1" stream_name="$2" video_source="$3"
    allocate_values publish "$label" rtmp "$stream_name"
    publisher_stream_id="$allocation_id"
    "$ffmpeg_bin" -nostdin -hide_banner -loglevel error -re \
        -f lavfi -i "$video_source" -f lavfi -i sine=frequency=1000:sample_rate=44100 \
        -map 0:v:0 -map 1:a:0 -c:v libx264 -preset ultrafast -tune zerolatency \
        -pix_fmt yuv420p -g 25 -keyint_min 25 -sc_threshold 0 -c:a aac -b:a 96k -ac 2 \
        -f flv "$allocation_url" >"$work_dir/${label}.log" 2>&1 &
    publisher_pid=$!
    wait_runtime "$publisher_stream_id" publisher rtmp "$stream_name" streaming
}

stop_publisher() {
    local stream_name="$1"
    kill -INT "$publisher_pid"
    wait "$publisher_pid" 2>/dev/null || true
    publisher_pid=""
    wait_runtime "$publisher_stream_id" publisher rtmp "$stream_name" stopped
}

probe_player() {
    local label="$1" protocol="$2" url="$3"
    local -a options=()
    [[ "$protocol" == rtsp ]] && options=(-rtsp_transport tcp)
    timeout 12s "$ffmpeg_bin" -nostdin -hide_banner -loglevel error "${options[@]}" \
        -i "$url" -t 0.8 -map 0:v:0 -f null - >"$work_dir/${label}.log" 2>&1
}

establish_hls() {
    local label="$1" initial_url="$2" headers status location
    headers="$work_dir/${label}_redirect_headers.txt"
    status="$(curl --noproxy '*' -sS --connect-timeout 1 --max-time 5 --max-redirs 0 \
        -D "$headers" -o /dev/null -w '%{http_code}' "$initial_url")"
    [[ "$status" == 307 ]]
    location="$(sed -n 's/^[Ll]ocation:[[:space:]]*//p' "$headers" | tr -d '\r' | tail -1)"
    python3 - "$initial_url" "$location" <<'PY'
import sys
import urllib.parse
assert "stream_id=" not in sys.argv[2] and "session=" in sys.argv[2]
print(urllib.parse.urljoin(sys.argv[1], sys.argv[2]))
PY
}

fetch_hls() {
    local label="$1" url="$2" playlist segment segment_uri segment_url
    playlist="$work_dir/${label}.m3u8"
    segment="$work_dir/${label}.segment"
    curl --noproxy '*' -fsS --connect-timeout 1 --max-time 8 "$url" >"$playlist"
    segment_uri="$(grep -E '^[^#].*\.(ts|m4s)(\?.*)?$' "$playlist" | head -1)"
    [[ -n "$segment_uri" && "$segment_uri" == *session=* ]]
    segment_url="$(python3 - "$url" "$segment_uri" <<'PY'
import sys
import urllib.parse
print(urllib.parse.urljoin(sys.argv[1], sys.argv[2]))
PY
)"
    curl --noproxy '*' -fsS --connect-timeout 1 --max-time 8 "$segment_url" >"$segment"
    [[ -s "$segment" ]]
}

wait_hls_endlist() {
    local label="$1" url="$2" playlist
    playlist="$work_dir/${label}_endlist.m3u8"
    for _ in $(seq 1 150); do
        if curl --noproxy '*' -fsS --connect-timeout 1 --max-time 3 "$url" >"$playlist" 2>/dev/null && \
            grep -Fq '#EXT-X-ENDLIST' "$playlist"; then
            return
        fi
        sleep 0.1
    done
    echo "HLS viewer did not reach ENDLIST: $label" >&2
    return 1
}

wait_hls_stopped() {
    local id="$1" stream_name="$2"
    wait_runtime "$id" output hls "$stream_name" stopped 350
}

start_services() {
    if [[ -z "$signaling_bin" ]]; then
        (cd "$script_dir/../signaling" && go build -o "$work_dir/signaling" .)
        signaling_bin="$work_dir/signaling"
    else
        signaling_bin="$(realpath "$signaling_bin")"
    fi
    "$signaling_bin" --sip-listen 127.0.0.1:15210 --sip-advertise 127.0.0.1:15210 \
        --http-listen "127.0.0.1:$signaling_port" --database "$database" >"$work_dir/signaling.log" 2>&1 &
    signaling_pid=$!
    wait_http "http://127.0.0.1:$signaling_port/" "$signaling_pid" "$work_dir/signaling.log"
    "$server_bin" --rtmp-port "$rtmp_port" --rtsp-port "$rtsp_port" --http-port "$http_port" \
        --signaling-url "http://127.0.0.1:$signaling_port" --server-id lifecycle-soak \
        --control-url "http://127.0.0.1:$http_port" --media-ip 127.0.0.1 >"$work_dir/server.log" 2>&1 &
    server_pid=$!
    wait_http "http://127.0.0.1:$http_port/" "$server_pid" "$work_dir/server.log"
}

run_viewer_churn() {
    local stream_name=live/soak round protocol label
    local -a hls_churn_ids=() hls_churn_urls=()
    start_publisher soak_publisher "$stream_name" 'testsrc=size=320x180:rate=25'
    sample_resources warmup
    for round in $(seq 1 "$soak_rounds"); do
        for protocol in rtsp rtmp http-flv; do
            label="soak_${protocol//-/_}_$round"
            allocate_values play "$label" "$protocol" "$stream_name"
            probe_player "$label" "$protocol" "$allocation_url"
            wait_runtime "$allocation_id" output "$protocol" "$stream_name" stopped
        done
        label="soak_hls_$round"
        allocate_values play "$label" hls "$stream_name"
        hls_churn_ids+=("$allocation_id")
        allocation_url="$(establish_hls "$label" "$allocation_url")"
        hls_churn_urls+=("$allocation_url")
        fetch_hls "$label" "$allocation_url"
        if (( round % 20 == 0 || round == soak_rounds )); then sample_resources "churn-$round"; fi
    done
    for allocation_id in "${hls_churn_ids[@]}"; do wait_hls_stopped "$allocation_id" "$stream_name"; done
    for allocation_url in "${hls_churn_urls[@]}"; do
        [[ "$(curl --noproxy '*' -sS --connect-timeout 1 --max-time 5 -o /dev/null -w '%{http_code}' "$allocation_url")" == 403 ]]
    done
    stop_publisher "$stream_name"
    sample_resources churn-final
}

run_mixed_batches() {
    local stream_name=live/mixed-soak batch index protocol label
    local -a batch_pids=() batch_ids=() batch_protocols=() batch_labels=() mixed_hls_ids=()
    start_publisher mixed_publisher "$stream_name" 'testsrc2=size=320x180:rate=25'
    for batch in $(seq 1 5); do
        batch_pids=() batch_ids=() batch_protocols=() batch_labels=()
        for protocol in rtsp rtmp http-flv; do
            for index in 1 2; do
                label="mixed_${batch}_${protocol//-/_}_$index"
                allocate_values play "$label" "$protocol" "$stream_name"
                local id="$allocation_id" url="$allocation_url"
                local -a options=()
                [[ "$protocol" == rtsp ]] && options=(-rtsp_transport tcp)
                timeout 15s "$ffmpeg_bin" -nostdin -hide_banner -loglevel error "${options[@]}" \
                    -i "$url" -t 1 -map 0:v:0 -f null - >"$work_dir/${label}.log" 2>&1 &
                batch_pids+=("$!") batch_ids+=("$id") batch_protocols+=("$protocol") batch_labels+=("$label")
            done
        done
        for index in 1 2; do
            label="mixed_${batch}_hls_$index"
            allocate_values play "$label" hls "$stream_name"
            mixed_hls_ids+=("$allocation_id")
            allocation_url="$(establish_hls "$label" "$allocation_url")"
            fetch_hls "$label" "$allocation_url"
        done
        for index in "${!batch_pids[@]}"; do
            wait "${batch_pids[$index]}"
            wait_runtime "${batch_ids[$index]}" output "${batch_protocols[$index]}" "$stream_name" stopped
        done
        sample_resources "mixed-$batch"
    done
    for allocation_id in "${mixed_hls_ids[@]}"; do wait_hls_stopped "$allocation_id" "$stream_name"; done
    stop_publisher "$stream_name"
    sample_resources mixed-final
}

run_source_replacement() {
    local stream_name=live/generation-soak generation protocol label
    local previous_hls_url="" previous_segment="" previous_generation=0
    local -a replacement_hls_ids=() replacement_hls_urls=()
    for generation in $(seq 1 "$replacement_rounds"); do
        start_publisher "generation_publisher_$generation" "$stream_name" "testsrc=size=320x180:rate=25,hue=h=$((generation * 23))"
        for protocol in rtsp rtmp http-flv; do
            label="generation_${generation}_${protocol//-/_}"
            allocate_values play "$label" "$protocol" "$stream_name"
            probe_player "$label" "$protocol" "$allocation_url"
            wait_runtime "$allocation_id" output "$protocol" "$stream_name" stopped
        done
        label="generation_${generation}_hls"
        allocate_values play "$label" hls "$stream_name"
        replacement_hls_ids+=("$allocation_id")
        allocation_url="$(establish_hls "$label" "$allocation_url")"
        replacement_hls_urls+=("$allocation_url")
        fetch_hls "$label" "$allocation_url"
        if [[ -n "$previous_hls_url" ]]; then
            wait_hls_endlist "generation_${previous_generation}_retained" "$previous_hls_url"
            fetch_hls "generation_${previous_generation}_retained" "$previous_hls_url"
            cmp "$previous_segment" "$work_dir/generation_${previous_generation}_retained.segment"
            if cmp -s "$previous_segment" "$work_dir/${label}.segment"; then
                echo "source generation $generation reused generation $previous_generation HLS payload" >&2
                return 1
            fi
        fi
        previous_hls_url="$allocation_url"
        previous_segment="$work_dir/generation_${generation}_hls.segment"
        previous_generation="$generation"
        stop_publisher "$stream_name"
        wait_hls_endlist "generation_${generation}_ended" "$allocation_url"
        sample_resources "generation-$generation"
    done
    for allocation_id in "${replacement_hls_ids[@]}"; do wait_hls_stopped "$allocation_id" "$stream_name"; done
    sample_resources replacement-final
}

start_services
run_viewer_churn
run_mixed_batches
run_source_replacement

python3 - "$ids_file" <<'PY'
import pathlib
import sys
import uuid

ids = [line.strip() for line in pathlib.Path(sys.argv[1]).read_text().splitlines() if line.strip()]
parsed = [uuid.UUID(value) for value in ids]
assert all(value.version == 4 and str(value) == text for value, text in zip(parsed, ids))
assert len(ids) == len(set(ids)), "stream_id reused during soak"
print(f"stream identities: {len(ids)} unique")
PY

echo "lifecycle soak passed: rounds=$soak_rounds replacement_generations=$replacement_rounds"
