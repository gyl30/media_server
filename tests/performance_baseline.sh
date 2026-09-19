#!/usr/bin/env bash
set -euo pipefail

server_bin="${1:-./build-profile/media_server}"
work_dir="${2:-$(mktemp -d /tmp/media_server_performance.XXXXXX)}"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
signaling_bin="${SIGNALING_BIN:-}"
ffmpeg_bin="${FFMPEG_BIN:-ffmpeg}"
warmup_seconds="${MEDIA_SERVER_PERF_WARMUP_SECONDS:-10}"
measurement_seconds="${MEDIA_SERVER_PERF_MEASUREMENT_SECONDS:-30}"
runs="${MEDIA_SERVER_PERF_RUNS:-3}"
workloads="${MEDIA_SERVER_PERF_WORKLOADS:-idle publish-only rtsp-1 rtsp-4 rtsp-8 rtmp-1 rtmp-4 rtmp-8 http-flv-1 http-flv-4 http-flv-8 hls-1 hls-4 hls-8 mixed-8}"
profile_tool="${MEDIA_SERVER_PERF_PROFILE_TOOL:-none}"
worker_threads="${MEDIA_SERVER_PERF_THREADS:-}"
signaling_port="${MEDIA_SERVER_PERF_SIGNALING_PORT:-19310}"
rtmp_port="${MEDIA_SERVER_PERF_RTMP_PORT:-19610}"
rtsp_port="${MEDIA_SERVER_PERF_RTSP_PORT:-18810}"
http_port="${MEDIA_SERVER_PERF_HTTP_PORT:-18310}"
sip_port="${MEDIA_SERVER_PERF_SIP_PORT:-15310}"

[[ "$warmup_seconds" =~ ^[1-9][0-9]*$ ]]
[[ "$measurement_seconds" =~ ^[1-9][0-9]*$ ]]
[[ "$runs" =~ ^[1-9][0-9]*$ ]]
[[ -z "$worker_threads" || "$worker_threads" =~ ^[1-9][0-9]*$ ]]
case "$profile_tool" in
    none | callgrind | massif) ;;
    *)
        echo "unsupported profile tool: $profile_tool" >&2
        exit 2
        ;;
esac

mkdir -p "$work_dir"
work_dir="$(cd "$work_dir" && pwd)"
server_bin="$(realpath "$server_bin")"
owned_pids_file="$work_dir/owned_pids.tsv"
runs_file="$work_dir/runs.tsv"
summary_file="$work_dir/summary.tsv"
raw_samples_file="$work_dir/raw_samples.tsv"
: >"$owned_pids_file"
printf 'workload\tviewers\trun\tcpu_percent\trss_kb\tvmsize_kb\tvmhwm_kb\trss_anon_kb\trss_file_kb\trss_shmem_kb\tthreads\tfd\tctxt_per_second\n' >"$runs_file"
printf 'workload\trun\tsecond\trss_kb\tvmsize_kb\tvmhwm_kb\trss_anon_kb\trss_file_kb\trss_shmem_kb\tthreads\tfd\tvoluntary_ctxt\tnonvoluntary_ctxt\n' >"$raw_samples_file"

main_shell_pid="$BASHPID"
signaling_pid=""
server_pid=""
publisher_pid=""
declare -a player_pids=()
declare -a player_ids=()
declare -a player_protocols=()
declare -a player_labels=()

stop_run() {
    set +e
    local pid
    for pid in "${player_pids[@]}"; do kill -TERM "$pid" 2>/dev/null; done
    [[ -n "$publisher_pid" ]] && kill -INT "$publisher_pid" 2>/dev/null
    [[ -n "$server_pid" ]] && kill -TERM "$server_pid" 2>/dev/null
    [[ -n "$signaling_pid" ]] && kill -TERM "$signaling_pid" 2>/dev/null
    for pid in "${player_pids[@]}"; do wait "$pid" 2>/dev/null; done
    [[ -n "$publisher_pid" ]] && wait "$publisher_pid" 2>/dev/null
    [[ -n "$server_pid" ]] && wait "$server_pid" 2>/dev/null
    [[ -n "$signaling_pid" ]] && wait "$signaling_pid" 2>/dev/null
    player_pids=()
    player_ids=()
    player_protocols=()
    player_labels=()
    publisher_pid=""
    server_pid=""
    signaling_pid=""
    set -e
}

cleanup() {
    [[ "$BASHPID" == "$main_shell_pid" ]] || return
    stop_run
}
trap cleanup EXIT

wait_http() {
    local url="$1" pid="$2" phase="$3"
    for _ in $(seq 1 600); do
        if curl --noproxy '*' -sS --max-time 1 -o /dev/null "$url" 2>/dev/null; then
            kill -0 "$pid" 2>/dev/null
            return
        fi
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "service endpoint failed (phase=$phase)" >&2
            return 1
        fi
        sleep 0.1
    done
    echo "service endpoint timeout (phase=$phase)" >&2
    return 1
}

allocate() {
    local operation="$1" protocol="$2" stream_name="$3" label="$4"
    local response="$5" body status
    printf -v body '{"protocol":"%s","stream_name":"%s"}' "$protocol" "$stream_name"
    status="$(curl --noproxy '*' -sS --connect-timeout 1 --max-time 5 -o "$response" -w '%{http_code}' \
        -H 'Content-Type: application/json' --data-binary "$body" \
        "http://127.0.0.1:$signaling_port/api/$operation/allocations")"
    if [[ "$status" != 201 ]]; then
        echo "allocation failed (operation=$operation protocol=$protocol label=$label status=$status)" >&2
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

wait_runtime() {
    local stream_id="$1" kind="$2" protocol="$3" stream_name="$4" state="$5" label="$6"
    local response="$7"
    for _ in $(seq 1 600); do
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
    echo "runtime timeout (label=$label kind=$kind protocol=$protocol state=$state)" >&2
    return 1
}

establish_hls() {
    local initial_url="$1" label="$2" run_dir="$3" headers status location
    headers="$run_dir/${label}_redirect_headers.txt"
    if ! status="$(curl --noproxy '*' -sS --connect-timeout 1 --max-time 5 --max-redirs 0 \
        -D "$headers" -o /dev/null -w '%{http_code}' "$initial_url" \
        2>"$run_dir/${label}_redirect_error.log")"; then
        echo "HLS redirect request failed (label=$label)" >&2
        return 1
    fi
    if [[ "$status" != 307 ]]; then
        echo "HLS redirect failed (label=$label status=$status)" >&2
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

start_services() {
    local run_dir="$1"
    (
        trap - EXIT
        exec "$signaling_bin" --sip-listen "127.0.0.1:$sip_port" --sip-advertise "127.0.0.1:$sip_port" \
            --http-listen "127.0.0.1:$signaling_port" --database "$run_dir/signaling.db"
    ) >"$run_dir/signaling.log" 2>&1 &
    signaling_pid=$!
    printf 'signaling\t%s\n' "$signaling_pid" >>"$owned_pids_file"
    wait_http "http://127.0.0.1:$signaling_port/" "$signaling_pid" signaling

    local -a server_command=("$server_bin")
    case "$profile_tool" in
        callgrind)
            server_command=(valgrind --quiet --tool=callgrind --instr-atstart=no --collect-jumps=yes \
                "--callgrind-out-file=$run_dir/callgrind.out.%p" "$server_bin")
            ;;
        massif)
            server_command=(valgrind --quiet --tool=massif --time-unit=ms \
                "--massif-out-file=$run_dir/massif.out.%p" "$server_bin")
            ;;
    esac
    local -a server_arguments=(
        --rtmp-port "$rtmp_port"
        --rtsp-port "$rtsp_port"
        --http-port "$http_port"
        --signaling-url "http://127.0.0.1:$signaling_port"
        --server-id performance-baseline
        --control-url "http://127.0.0.1:$http_port"
        --media-ip 127.0.0.1
    )
    if [[ -n "$worker_threads" ]]; then
        server_arguments+=(--threads "$worker_threads")
    fi
    (
        trap - EXIT
        exec "${server_command[@]}" "${server_arguments[@]}"
    ) >"$run_dir/server.log" 2>&1 &
    server_pid=$!
    printf 'media_server\t%s\n' "$server_pid" >>"$owned_pids_file"
    wait_http "http://127.0.0.1:$http_port/" "$server_pid" media_server
}

snapshot_server() {
    local label="$1" run_dir="$2" snapshot_dir="$2/snapshots"
    mkdir -p "$snapshot_dir"
    ps -T -p "$server_pid" -o pid,tid,comm >"$snapshot_dir/${label}_threads.txt"
    {
        for task in /proc/"$server_pid"/task/*; do
            local tid="${task##*/}"
            printf '%s\t' "$tid"
            cat "$task/comm"
        done
    } >"$snapshot_dir/${label}_task_comm.txt"
    cat "/proc/$server_pid/status" >"$snapshot_dir/${label}_status.txt"
    cat "/proc/$server_pid/smaps_rollup" >"$snapshot_dir/${label}_smaps_rollup.txt"
    cat "/proc/$server_pid/maps" >"$snapshot_dir/${label}_maps.txt"
}

start_publisher() {
    local stream_name="$1" run_dir="$2" values
    values="$(allocate publish rtmp "$stream_name" publisher "$run_dir/publisher_allocation.json")"
    mapfile -t allocation_values <<<"$values"
    local stream_id="${allocation_values[0]}" url="${allocation_values[1]}"
    (
        trap - EXIT
        exec "$ffmpeg_bin" -nostdin -hide_banner -loglevel error -re \
            -f lavfi -i testsrc2=size=1280x720:rate=30 \
            -f lavfi -i sine=frequency=1000:sample_rate=44100 \
            -map 0:v:0 -map 1:a:0 -c:v libx264 -preset ultrafast -tune zerolatency \
            -pix_fmt yuv420p -g 30 -keyint_min 30 -sc_threshold 0 \
            -c:a aac -b:a 96k -ac 2 -f flv "$url"
    ) >"$run_dir/publisher.log" 2>&1 &
    publisher_pid=$!
    printf 'publisher\t%s\n' "$publisher_pid" >>"$owned_pids_file"
    wait_runtime "$stream_id" publisher rtmp "$stream_name" streaming publisher \
        "$run_dir/publisher_runtime.json"
}

start_player() {
    local protocol="$1" label="$2" stream_name="$3" run_dir="$4" values url
    values="$(allocate play "$protocol" "$stream_name" "$label" "$run_dir/${label}_allocation.json")"
    mapfile -t allocation_values <<<"$values"
    local stream_id="${allocation_values[0]}"
    url="${allocation_values[1]}"
    if [[ "$protocol" == hls ]]; then
        url="$(establish_hls "$url" "$label" "$run_dir")"
    fi
    local -a input_options=()
    [[ "$protocol" == rtsp ]] && input_options=(-rtsp_transport tcp)
    local timeout_seconds=$((warmup_seconds + measurement_seconds + 60))
    (
        trap - EXIT
        exec timeout --signal=TERM "${timeout_seconds}s" "$ffmpeg_bin" -nostdin -hide_banner \
            -loglevel error "${input_options[@]}" -i "$url" -map 0:v:0 -f null -
    ) >"$run_dir/${label}.log" 2>&1 &
    local pid=$!
    player_pids+=("$pid")
    player_ids+=("$stream_id")
    player_protocols+=("$protocol")
    player_labels+=("$label")
    printf 'player\t%s\n' "$pid" >>"$owned_pids_file"
}

check_players() {
    local index
    for index in "${!player_pids[@]}"; do
        if ! kill -0 "${player_pids[$index]}" 2>/dev/null; then
            wait "${player_pids[$index]}" 2>/dev/null || true
            echo "player exited during workload (protocol=${player_protocols[$index]} index=$index)" >&2
            return 1
        fi
    done
}

status_value() {
    local key="$1"
    awk -v key="$key" '$1 == key ":" {print $2}' "/proc/$server_pid/status"
}

context_switches() {
    awk '/^voluntary_ctxt_switches:/{voluntary+=$2} /^nonvoluntary_ctxt_switches:/{nonvoluntary+=$2} END{print voluntary + nonvoluntary}' \
        /proc/"$server_pid"/task/*/status
}

context_switch_values() {
    awk '/^voluntary_ctxt_switches:/{voluntary+=$2} /^nonvoluntary_ctxt_switches:/{nonvoluntary+=$2} END{print voluntary, nonvoluntary}' \
        /proc/"$server_pid"/task/*/status
}

sample_workload() {
    local workload="$1" run="$2" viewers="$3" run_dir="$4"
    local samples="$run_dir/samples.tsv" second fd
    local begin_ticks end_ticks begin_ns end_ns begin_ctxt end_ctxt
    local -a ctxt_values=()
    printf 'second\trss_kb\tvmsize_kb\tvmhwm_kb\trss_anon_kb\trss_file_kb\trss_shmem_kb\tthreads\tfd\tvoluntary_ctxt\tnonvoluntary_ctxt\n' >"$samples"
    begin_ticks="$(awk '{print $14 + $15}' "/proc/$server_pid/stat")"
    begin_ctxt="$(context_switches)"
    begin_ns="$(date +%s%N)"
    for second in $(seq 1 "$measurement_seconds"); do
        check_players
        fd="$(find "/proc/$server_pid/fd" -mindepth 1 -maxdepth 1 -type l 2>/dev/null | wc -l)"
        read -r -a ctxt_values <<<"$(context_switch_values)"
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$second" "$(status_value VmRSS)" "$(status_value VmSize)" "$(status_value VmHWM)" \
            "$(status_value RssAnon)" "$(status_value RssFile)" "$(status_value RssShmem)" \
            "$(status_value Threads)" "$fd" "${ctxt_values[0]}" "${ctxt_values[1]}" \
            | tee -a "$samples" \
            | awk -v workload="$workload" -v run="$run" 'BEGIN{OFS="\t"} {print workload, run, $0}' \
            >>"$raw_samples_file"
        sleep 1
    done
    end_ns="$(date +%s%N)"
    end_ctxt="$(context_switches)"
    end_ticks="$(awk '{print $14 + $15}' "/proc/$server_pid/stat")"
    python3 - "$samples" "$workload" "$viewers" "$run" "$begin_ticks" "$end_ticks" \
        "$begin_ns" "$end_ns" "$(getconf CLK_TCK)" "$begin_ctxt" "$end_ctxt" >>"$runs_file" <<'PY'
import csv
import statistics
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    rows = list(csv.DictReader(source, delimiter="\t"))
elapsed = (int(sys.argv[8]) - int(sys.argv[7])) / 1_000_000_000
cpu = (int(sys.argv[6]) - int(sys.argv[5])) / int(sys.argv[9]) / elapsed * 100
ctxt = (int(sys.argv[11]) - int(sys.argv[10])) / elapsed
fields = ["rss_kb", "vmsize_kb", "vmhwm_kb", "rss_anon_kb", "rss_file_kb",
          "rss_shmem_kb", "threads", "fd"]
medians = [statistics.median(int(row[field]) for row in rows) for field in fields]
print("\t".join([sys.argv[2], sys.argv[3], sys.argv[4], f"{cpu:.3f}",
                 *(f"{value:.1f}" for value in medians), f"{ctxt:.3f}"]))
PY
}

workload_viewers() {
    case "$1" in
        idle | publish-only) echo 0 ;;
        mixed-8) echo 8 ;;
        *) echo "${1##*-}" ;;
    esac
}

start_workload_players() {
    local workload="$1" stream_name="$2" run_dir="$3" protocol count index
    case "$workload" in
        idle | publish-only) return ;;
        mixed-8)
            for protocol in rtsp rtmp http-flv hls; do
                for index in 1 2; do
                    start_player "$protocol" "${protocol//-/_}_$index" "$stream_name" "$run_dir"
                done
            done
            ;;
        rtsp-* | rtmp-* | http-flv-* | hls-*)
            protocol="${workload%-*}"
            count="${workload##*-}"
            for index in $(seq 1 "$count"); do
                start_player "$protocol" "${protocol//-/_}_$index" "$stream_name" "$run_dir"
            done
            ;;
        *)
            echo "unsupported workload: $workload" >&2
            return 2
            ;;
    esac
}

run_workload() {
    local workload="$1" run="$2" viewers stream_name run_dir index
    viewers="$(workload_viewers "$workload")"
    stream_name="live/performance/${workload//-/_}/$run"
    run_dir="$work_dir/${workload}_run_$run"
    mkdir -p "$run_dir"
    player_pids=()
    player_ids=()
    player_protocols=()
    player_labels=()
    printf '%s\n' "$(cat /proc/loadavg)" >"$run_dir/loadavg_before.txt"
    echo "performance workload start (name=$workload run=$run viewers=$viewers)"
    start_services "$run_dir"
    snapshot_server after-server "$run_dir"
    if [[ "$workload" != idle ]]; then
        start_publisher "$stream_name" "$run_dir"
        snapshot_server after-publisher "$run_dir"
    fi
    start_workload_players "$workload" "$stream_name" "$run_dir"
    for index in "${!player_pids[@]}"; do
        wait_runtime "${player_ids[$index]}" output "${player_protocols[$index]}" "$stream_name" streaming \
            "${player_labels[$index]}" "$run_dir/${player_labels[$index]}_runtime.json"
    done
    snapshot_server after-players "$run_dir"
    sleep "$warmup_seconds"
    check_players
    snapshot_server steady "$run_dir"
    if [[ "$profile_tool" == callgrind ]]; then
        callgrind_control -z "$server_pid" >/dev/null
        callgrind_control -i on "$server_pid" >/dev/null
    fi
    sample_workload "$workload" "$run" "$viewers" "$run_dir"
    if [[ "$profile_tool" == callgrind ]]; then
        callgrind_control -i off "$server_pid" >/dev/null
        callgrind_control --dump="${workload}_run_$run" "$server_pid" >/dev/null
    fi
    printf '%s\n' "$(cat /proc/loadavg)" >"$run_dir/loadavg_after.txt"
    stop_run
    echo "performance workload complete (name=$workload run=$run)"
}

summarize() {
    python3 - "$runs_file" "$summary_file" <<'PY'
import csv
import statistics
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    rows = list(csv.DictReader(source, delimiter="\t"))
groups = {}
for row in rows:
    groups.setdefault(row["workload"], []).append(row)
fields = ["rss_kb", "vmsize_kb", "vmhwm_kb", "rss_anon_kb", "rss_file_kb",
          "rss_shmem_kb", "threads", "fd", "ctxt_per_second"]
with open(sys.argv[2], "w", encoding="utf-8", newline="") as output:
    writer = csv.writer(output, delimiter="\t", lineterminator="\n")
    writer.writerow(["workload", "viewers", "runs", "cpu_min", "cpu_median", "cpu_max", *fields])
    for workload, group in groups.items():
        cpu = [float(row["cpu_percent"]) for row in group]
        medians = [statistics.median(float(row[field]) for row in group) for field in fields]
        writer.writerow([workload, group[0]["viewers"], len(group), f"{min(cpu):.3f}",
                         f"{statistics.median(cpu):.3f}", f"{max(cpu):.3f}",
                         *(f"{value:.1f}" for value in medians)])
PY
}

if [[ -z "$signaling_bin" ]]; then
    (cd "$script_dir/../signaling" && go build -o "$work_dir/signaling" .)
    signaling_bin="$work_dir/signaling"
else
    signaling_bin="$(realpath "$signaling_bin")"
fi

for workload in $workloads; do
    for run in $(seq 1 "$runs"); do
        run_workload "$workload" "$run"
    done
done
summarize

residual=0
for pid in "${player_pids[@]}" "$publisher_pid" "$server_pid" "$signaling_pid"; do
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
        residual=$((residual + 1))
    fi
done
echo "performance baseline complete (owned_residual_pids=$residual output=$work_dir)"
(( residual == 0 ))
