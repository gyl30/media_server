#!/usr/bin/env bash
set -euo pipefail

server_bin="${1:-./build/media_server}"
work_dir="${2:-$(mktemp -d /tmp/media_server_fanout.XXXXXX)}"
viewers="${3:-1}"
profile="${4:-low}"
duration_seconds="${MEDIA_SERVER_FANOUT_DURATION_SECONDS:-15}"
ramp_per_second="${MEDIA_SERVER_FANOUT_RAMP_PER_SECOND:-50}"
shards="${MEDIA_SERVER_FANOUT_SHARDS:-1}"
profile_tool="${MEDIA_SERVER_FANOUT_PROFILE_TOOL:-none}"
signaling_bin="${SIGNALING_BIN:-}"
ffmpeg_bin="${FFMPEG_BIN:-ffmpeg}"
base_port="${MEDIA_SERVER_FANOUT_BASE_PORT:-20000}"
signaling_port=$((base_port + 0))
rtmp_port=$((base_port + 1))
rtsp_port=$((base_port + 2))
http_port=$((base_port + 3))
sip_port=$((base_port + 4))

[[ "$viewers" =~ ^[1-9][0-9]*$ ]]
[[ "$duration_seconds" =~ ^[1-9][0-9]*$ ]]
[[ "$ramp_per_second" =~ ^[1-9][0-9]*$ ]]
[[ "$shards" =~ ^[1-9][0-9]*$ ]]
(( viewers % shards == 0 )) || { echo "viewers must be divisible by shards" >&2; exit 2; }
shard_viewers=$((viewers / shards))
shard_ramp=$((ramp_per_second / shards))
(( shard_ramp < 1 )) && shard_ramp=1
estimated_shard_ramp_seconds=$(( (shard_viewers + shard_ramp - 1) / shard_ramp ))
case "$profile" in
    low | normal) ;;
    *)
        echo "unsupported profile: $profile" >&2
        exit 2
        ;;
esac
case "$profile_tool" in
    none | callgrind) ;;
    *)
        echo "unsupported profile tool: $profile_tool" >&2
        exit 2
        ;;
esac

mkdir -p "$work_dir"
work_dir="$(cd "$work_dir" && pwd)"
server_bin="$(realpath "$server_bin")"
generator_bin="$(realpath "$(dirname "$server_bin")/fanout_rtmp_generator")"
signaling_pid=""
server_pid=""
publisher_pid=""
declare -a generator_pids=()

cleanup() {
    set +e
    for pid in "${generator_pids[@]}" "${publisher_pid:-}" "${server_pid:-}" "${signaling_pid:-}"; do
        [[ -n "$pid" ]] && kill -TERM "$pid" 2>/dev/null
    done
    for pid in "${generator_pids[@]}" "${publisher_pid:-}" "${server_pid:-}" "${signaling_pid:-}"; do
        [[ -n "$pid" ]] && wait "$pid" 2>/dev/null
    done
    local residual=0
    for pid in "${generator_pids[@]}" "${publisher_pid:-}" "${server_pid:-}" "${signaling_pid:-}"; do
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            residual=$((residual + 1))
        fi
    done
    echo "owned_residual_pids=$residual"
    set -e
}
trap cleanup EXIT

wait_http() {
    local url="$1" pid="$2" phase="$3"
    for _ in $(seq 1 200); do
        if curl --noproxy '*' -sS --max-time 1 -o /dev/null "$url" 2>/dev/null; then
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

wait_publisher_streaming() {
    local response="$work_dir/publisher_runtime.json"
    for _ in $(seq 1 200); do
        if curl --noproxy '*' -fsS --max-time 1 "http://127.0.0.1:$signaling_port/api/runtimes" >"$response" 2>/dev/null && \
            python3 - "$response" "$stream_id" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    runtimes = json.load(source)["runtimes"]
for runtime in runtimes:
    if runtime.get("stream_id") == sys.argv[2] and runtime.get("state") == "streaming":
        raise SystemExit(0)
raise SystemExit(1)
PY
        then
            return
        fi
        sleep 0.1
    done
    echo "publisher did not reach streaming" >&2
    return 1
}

sample_process() {
    local label="$1" pid="$2" output="$3"
    local now_ns ticks rss vmsize threads fd
    now_ns="$(date +%s%N)"
    ticks="$(awk '{print $14 + $15}' "/proc/$pid/stat")"
    read -r rss vmsize threads < <(awk '$1 == "VmRSS:" {rss=$2} $1 == "VmSize:" {vmsize=$2} $1 == "Threads:" {threads=$2} END {print rss, vmsize, threads}' "/proc/$pid/status")
    fd="$(find "/proc/$pid/fd" -mindepth 1 -maxdepth 1 -type l 2>/dev/null | wc -l)"
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$label" "$now_ns" "$ticks" "$rss" "$vmsize" "$threads" "$fd" >>"$output"
}

sample_server_sockets() {
    local output="$1" now_ns established recvq_total recvq_nonzero recvq_max sendq_total sendq_nonzero sendq_max
    now_ns="$(date +%s%N)"
    read -r established recvq_total recvq_nonzero recvq_max sendq_total sendq_nonzero sendq_max < <(
        ss -tanH 2>/dev/null | awk -v port="$rtmp_port" '
            $1 == "ESTAB" {
                local_port=$4
                sub(/^.*:/, "", local_port)
                if (local_port == port) {
                    established++
                    recvq += $2
                    if ($2 > 0) recv_nonzero++
                    if ($2 > recv_max) recv_max=$2
                    sendq += $3
                    if ($3 > 0) send_nonzero++
                    if ($3 > send_max) send_max=$3
                }
            }
            END { print established + 0, recvq + 0, recv_nonzero + 0, recv_max + 0, sendq + 0, send_nonzero + 0, send_max + 0 }')
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$now_ns" "$established" "$recvq_total" "$recvq_nonzero" "$recvq_max" "$sendq_total" "$sendq_nonzero" "$sendq_max" >>"$output"
}

sample_client_sockets() {
    local output="$1" now_ns established recvq_total recvq_nonzero recvq_max sendq_total sendq_nonzero sendq_max
    now_ns="$(date +%s%N)"
    read -r established recvq_total recvq_nonzero recvq_max sendq_total sendq_nonzero sendq_max < <(
        ss -tanH 2>/dev/null | awk -v port="$rtmp_port" '
            $1 == "ESTAB" {
                peer_port=$5
                sub(/^.*:/, "", peer_port)
                if (peer_port == port) {
                    established++
                    recvq += $2
                    if ($2 > 0) recv_nonzero++
                    if ($2 > recv_max) recv_max=$2
                    sendq += $3
                    if ($3 > 0) send_nonzero++
                    if ($3 > send_max) send_max=$3
                }
            }
            END { print established + 0, recvq + 0, recv_nonzero + 0, recv_max + 0, sendq + 0, send_nonzero + 0, send_max + 0 }')
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$now_ns" "$established" "$recvq_total" "$recvq_nonzero" "$recvq_max" "$sendq_total" "$sendq_nonzero" "$sendq_max" >>"$output"
}

sample_threads() {
    local label="$1" pid="$2" output="$3" now_ns tid ticks comm task
    now_ns="$(date +%s%N)"
    for task in /proc/"$pid"/task/*; do
        tid="${task##*/}"
        if ! ticks="$(awk '{print $14 + $15}' "$task/stat" 2>/dev/null)" || [[ -z "$ticks" ]]; then
            continue
        fi
        if ! comm="$(cat "$task/comm" 2>/dev/null)"; then
            continue
        fi
        printf '%s\t%s\t%s\t%s\t%s\n' "$label" "$now_ns" "$tid" "$comm" "$ticks" >>"$output"
    done
}

if [[ -z "$signaling_bin" ]]; then
    (cd "$(dirname "$0")/../signaling" && go build -o "$work_dir/signaling" .)
    signaling_bin="$work_dir/signaling"
else
    signaling_bin="$(realpath "$signaling_bin")"
fi

printf 'label\ttime_ns\tticks\trss_kb\tvmsize_kb\tthreads\tfd\n' >"$work_dir/server_samples.tsv"
printf 'label\ttime_ns\tticks\trss_kb\tvmsize_kb\tthreads\tfd\n' >"$work_dir/generator_samples.tsv"
printf 'time_ns\testablished\trecvq_total\trecvq_nonzero\trecvq_max\tsendq_total\tsendq_nonzero\tsendq_max\n' >"$work_dir/server_socket_samples.tsv"
printf 'time_ns\testablished\trecvq_total\trecvq_nonzero\trecvq_max\tsendq_total\tsendq_nonzero\tsendq_max\n' >"$work_dir/client_socket_samples.tsv"
printf 'label\ttime_ns\ttid\tcomm\tticks\n' >"$work_dir/server_threads.tsv"
printf 'label\ttime_ns\ttid\tcomm\tticks\n' >"$work_dir/generator_threads.tsv"
printf '%s\n' "$(cat /proc/loadavg)" >"$work_dir/loadavg_before.txt"
printf '%s\n' "$(ulimit -n)" >"$work_dir/ulimit_n.txt"
cat /proc/sys/net/ipv4/ip_local_port_range >"$work_dir/ip_local_port_range.txt"

(
    trap - EXIT
    exec "$signaling_bin" --sip-listen "127.0.0.1:$sip_port" --sip-advertise "127.0.0.1:$sip_port" \
        --http-listen "127.0.0.1:$signaling_port" --database "$work_dir/signaling.db"
) >"$work_dir/signaling.log" 2>&1 &
signaling_pid=$!
wait_http "http://127.0.0.1:$signaling_port/" "$signaling_pid" signaling

server_command=("$server_bin")
if [[ "$profile_tool" == callgrind ]]; then
    server_command=(valgrind --quiet --tool=callgrind --instr-atstart=no "--callgrind-out-file=$work_dir/callgrind.out.%p" "$server_bin")
fi
(
    trap - EXIT
    exec "${server_command[@]}" --rtmp-port "$rtmp_port" --rtsp-port "$rtsp_port" --http-port "$http_port" \
        --signaling-url "http://127.0.0.1:$signaling_port" --server-id fanout --control-url "http://127.0.0.1:$http_port" \
        --media-ip 127.0.0.1
) >"$work_dir/server.log" 2>&1 &
server_pid=$!
wait_http "http://127.0.0.1:$http_port/" "$server_pid" media_server

printf -v stream_name 'live/fanout/%s/%s' "$profile" "$viewers"
printf -v publish_body '{"protocol":"rtmp","stream_name":"%s"}' "$stream_name"
publish_response="$work_dir/publish.json"
curl --noproxy '*' -fsS --connect-timeout 1 --max-time 5 -H 'Content-Type: application/json' --data-binary "$publish_body" \
    "http://127.0.0.1:$signaling_port/api/publish/allocations" >"$publish_response"
stream_id="$(python3 - "$publish_response" <<'PY'
import json
import sys
print(json.load(open(sys.argv[1], encoding="utf-8"))["stream_id"])
PY
)"
publish_url="$(python3 - "$publish_response" <<'PY'
import json
import sys
print(json.load(open(sys.argv[1], encoding="utf-8"))["publish_url"])
PY
)"

if [[ "$profile" == low ]]; then
    video_input="testsrc2=size=320x180:rate=10"
    video_rate=(-b:v 220k)
else
    video_input="testsrc2=size=1280x720:rate=30"
    video_rate=()
fi
(
    trap - EXIT
    exec "$ffmpeg_bin" -nostdin -hide_banner -loglevel error -re \
        -f lavfi -i "$video_input" -f lavfi -i sine=frequency=1000:sample_rate=44100 \
        -map 0:v:0 -map 1:a:0 -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p \
        -g 30 -keyint_min 30 -sc_threshold 0 "${video_rate[@]}" -c:a aac -b:a 48k -ac 1 -t "$((estimated_shard_ramp_seconds + duration_seconds + 30))" \
        -f flv "$publish_url"
) >"$work_dir/publisher.log" 2>&1 &
publisher_pid=$!
wait_publisher_streaming

echo "fanout start (profile=$profile viewers=$viewers shards=$shards shard_viewers=$shard_viewers duration=${duration_seconds}s requested_ramp=${ramp_per_second}/s effective_ramp=$((shard_ramp * shards))/s shard_ramp=${shard_ramp}/s)"
if [[ "$profile_tool" == callgrind ]]; then
    callgrind_control -z "$server_pid" >/dev/null
    callgrind_control -i on "$server_pid" >/dev/null
fi
for ((shard = 0; shard < shards; ++shard)); do
    (
        trap - EXIT
        exec "$generator_bin" --signaling-url "http://127.0.0.1:$signaling_port" --stream-name "$stream_name" \
            --media-port "$rtmp_port" --viewers "$shard_viewers" --duration "$duration_seconds" --ramp-per-second "$shard_ramp"
    ) >"$work_dir/generator_${shard}.log" 2>&1 &
    generator_pids+=("$!")
done
while :; do
    any_generator_alive=0
    for pid in "${generator_pids[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            any_generator_alive=1
            break
        fi
    done
    (( any_generator_alive == 1 )) || break
    sample_process server "$server_pid" "$work_dir/server_samples.tsv"
    sample_server_sockets "$work_dir/server_socket_samples.tsv"
    sample_client_sockets "$work_dir/client_socket_samples.tsv"
    sample_threads server "$server_pid" "$work_dir/server_threads.tsv"
    for ((shard = 0; shard < shards; ++shard)); do
        pid="${generator_pids[$shard]}"
        if kill -0 "$pid" 2>/dev/null; then
            sample_process "generator_${shard}" "$pid" "$work_dir/generator_samples.tsv"
            sample_threads "generator_${shard}" "$pid" "$work_dir/generator_threads.tsv"
        fi
    done
    sleep 1
done
generator_status=0
for pid in "${generator_pids[@]}"; do
    wait "$pid" || generator_status=$?
done
if [[ "$profile_tool" == callgrind ]]; then
    callgrind_control -i off "$server_pid" >/dev/null
    callgrind_control --dump="fanout_${profile}_${viewers}" "$server_pid" >/dev/null
fi
printf '%s\n' "$(cat /proc/loadavg)" >"$work_dir/loadavg_after.txt"
sleep 1
curl --noproxy '*' -fsS --connect-timeout 1 --max-time 5 "http://127.0.0.1:$signaling_port/api/runtimes" \
    >"$work_dir/runtime_snapshot.json" 2>/dev/null || true

generator_log_paths=()
for ((shard = 0; shard < shards; ++shard)); do
    generator_log_paths+=("$work_dir/generator_${shard}.log")
done

python3 - "$work_dir/server_samples.tsv" "$work_dir/generator_samples.tsv" "$work_dir/server_socket_samples.tsv" "$work_dir/client_socket_samples.tsv" "$work_dir/server_threads.tsv" "$work_dir/generator_threads.tsv" "$work_dir/results.tsv" "$work_dir/thread_summary.tsv" "$work_dir/runtime_snapshot.json" "$work_dir/client_socket_summary.tsv" "${generator_log_paths[@]}" <<'PY'
import csv
import json
import statistics
import sys

def summarize_process(path, start_ns, end_ns, label=None):
    with open(path, encoding="utf-8") as source:
        rows = list(csv.DictReader(source, delimiter="\t"))
    rows = [row for row in rows if start_ns <= int(row["time_ns"]) <= end_ns and (label is None or row["label"] == label)]
    values = {}
    for key in ("rss_kb", "vmsize_kb", "threads", "fd"):
        values[key] = statistics.median(int(row[key]) for row in rows) if rows else 0
    if len(rows) >= 2:
        elapsed = (int(rows[-1]["time_ns"]) - int(rows[0]["time_ns"])) / 1_000_000_000
        values["cpu_percent"] = (int(rows[-1]["ticks"]) - int(rows[0]["ticks"])) / int(sysconf_clk_tck) / elapsed * 100
        values["sample_count"] = len(rows)
    else:
        values["cpu_percent"] = 0.0
        values["sample_count"] = len(rows)
    return values

sysconf_clk_tck = int(__import__("os").sysconf("SC_CLK_TCK"))

def percentile(values, fraction):
    values = sorted(values)
    if not values:
        return 0
    return values[int(fraction * (len(values) - 1))]

def summarize_sockets(path, start_ns, end_ns):
    with open(path, encoding="utf-8") as source:
        rows = list(csv.DictReader(source, delimiter="\t"))
    rows = [row for row in rows if start_ns <= int(row["time_ns"]) <= end_ns]
    result = {}
    for key in ("established", "recvq_total", "recvq_nonzero", "recvq_max", "sendq_total", "sendq_nonzero", "sendq_max"):
        values = [int(row[key]) for row in rows]
        result[key + "_median"] = statistics.median(values) if values else 0
        result[key + "_p95"] = percentile(values, 0.95)
        result[key + "_max"] = max(values) if values else 0
    result["sample_count"] = len(rows)
    return result

def thread_summary(path, start_ns, end_ns):
    with open(path, encoding="utf-8") as source:
        rows = list(csv.DictReader(source, delimiter="\t"))
    rows = [row for row in rows if start_ns <= int(row["time_ns"]) <= end_ns]
    by_tid = {}
    for row in rows:
        by_tid.setdefault((row["label"], row["tid"]), []).append(row)
    result = []
    for (label, tid), entries in by_tid.items():
        if len(entries) < 2:
            continue
        elapsed = (int(entries[-1]["time_ns"]) - int(entries[0]["time_ns"])) / 1_000_000_000
        cpu = (int(entries[-1]["ticks"]) - int(entries[0]["ticks"])) / sysconf_clk_tck / elapsed * 100
        result.append((cpu, f"{label}:{tid}", entries[-1]["comm"]))
    return sorted(result, reverse=True)

def log_values(path):
    with open(path, encoding="utf-8") as source:
        text = source.read()
    values = {}
    for token in text.split():
        if "=" in token:
            name, value = token.split("=", 1)
            if value.isdigit():
                values[name] = int(value)
    return values

generator_log_paths = sys.argv[11:]
shard_values = [log_values(path) for path in generator_log_paths]
required = ("measurement_start_unix_ns", "measurement_end_unix_ns", "allocated", "ready", "progressing", "runtime_failures", "non_progressing_viewers")
if any(any(name not in values for name in required) for values in shard_values):
    raise RuntimeError("missing generator result field")
start_ns = max(values["measurement_start_unix_ns"] for values in shard_values)
end_ns = min(values["measurement_end_unix_ns"] for values in shard_values)
if end_ns <= start_ns:
    raise RuntimeError("generator measurement windows do not overlap")
elapsed = (end_ns - start_ns) / 1_000_000_000
server_threads = thread_summary(sys.argv[5], start_ns, end_ns)
generator_threads = thread_summary(sys.argv[6], start_ns, end_ns)
server_sockets = summarize_sockets(sys.argv[3], start_ns, end_ns)
client_sockets = summarize_sockets(sys.argv[4], start_ns, end_ns)
server = summarize_process(sys.argv[1], start_ns, end_ns)
generators = [summarize_process(sys.argv[2], start_ns, end_ns, f"generator_{index}") for index in range(len(shard_values))]
if server["sample_count"] < 5 or server_sockets["sample_count"] < 5 or client_sockets["sample_count"] < 5:
    raise RuntimeError("global measurement window has insufficient samples")
generator_total_cpu = sum(item["cpu_percent"] for item in generators)
generator_max_shard_cpu = max(item["cpu_percent"] for item in generators)
generator_total_rss = sum(item["rss_kb"] for item in generators)
generator_total_vmsize = sum(item["vmsize_kb"] for item in generators)
generator_total_fd = sum(item["fd"] for item in generators)
generator_max_thread_cpu = generator_threads[0][0] if generator_threads else 0.0
aggregate = {name: sum(values[name] for values in shard_values) for name in required[2:]}
for name in ("received_video_bytes", "received_video_messages", "steady_video_bytes", "steady_video_messages"):
    aggregate[name] = sum(values.get(name, 0) for values in shard_values)
with open(sys.argv[7], "w", encoding="utf-8") as output:
    output.write("elapsed_seconds\tglobal_overlap_seconds\tshards\tserver_cpu_percent\tserver_rss_kb\tserver_vmsize_kb\tserver_threads\tserver_fd\tserver_established_median\tserver_established_p95\tserver_established_max\tserver_recvq_total_p95\tserver_recvq_total_max\tserver_sendq_total_p95\tserver_sendq_total_max\tserver_sendq_nonzero_p95\tserver_sendq_nonzero_max\tserver_sendq_max_p95\tserver_sendq_max_max\tgenerator_total_cpu_percent\tgenerator_max_shard_cpu_percent\tgenerator_total_rss_kb\tgenerator_total_vmsize_kb\tgenerator_total_fd\tserver_max_thread_cpu\tgenerator_max_thread_cpu\ttotal_allocated\ttotal_ready\ttotal_progressing\ttotal_runtime_failures\ttotal_non_progressing\n")
    fields = [elapsed, elapsed, len(shard_values), server["cpu_percent"], server["rss_kb"], server["vmsize_kb"], server["threads"], server["fd"], server_sockets["established_median"], server_sockets["established_p95"], server_sockets["established_max"], server_sockets["recvq_total_p95"], server_sockets["recvq_total_max"], server_sockets["sendq_total_p95"], server_sockets["sendq_total_max"], server_sockets["sendq_nonzero_p95"], server_sockets["sendq_nonzero_max"], server_sockets["sendq_max_p95"], server_sockets["sendq_max_max"], generator_total_cpu, generator_max_shard_cpu, generator_total_rss, generator_total_vmsize, generator_total_fd, server_threads[0][0] if server_threads else 0.0, generator_max_thread_cpu, aggregate["allocated"], aggregate["ready"], aggregate["progressing"], aggregate["runtime_failures"], aggregate["non_progressing_viewers"]]
    output.write("\t".join(f"{value:.3f}" if isinstance(value, float) else str(value) for value in fields) + "\n")
with open(sys.argv[8], "w", encoding="utf-8") as output:
    output.write("process\ttid\tcomm\tcpu_percent\n")
    for process, entries in (("server", server_threads), ("generator", generator_threads)):
        for cpu, tid, comm in entries:
            output.write("%s\t%s\t%s\t%.3f\n" % (process, tid, comm, cpu))
with open(sys.argv[10], "w", encoding="utf-8") as output:
    output.write("metric\tmedian\tp95\tmax\n")
    for key in ("recvq_total", "recvq_nonzero", "recvq_max", "sendq_total", "sendq_nonzero", "sendq_max"):
        output.write("%s\t%s\t%s\t%s\n" % (key, client_sockets[key + "_median"], client_sockets[key + "_p95"], client_sockets[key + "_max"]))
try:
    with open(sys.argv[9], encoding="utf-8") as source:
        runtimes = json.load(source).get("runtimes", [])
except (FileNotFoundError, json.JSONDecodeError):
    runtimes = []
runtime_errors = {}
for runtime in runtimes:
    if runtime.get("state") == "runtime_error":
        key = "/".join(str(runtime.get(field, "")) for field in ("kind", "state", "stage", "error"))
        runtime_errors[key] = runtime_errors.get(key, 0) + 1
for error, count in sorted(runtime_errors.items()):
    print("server_runtime_error=" + error + " count=" + str(count))
print("server_top_threads=" + ",".join(f"{tid}:{comm}:{cpu:.1f}%" for cpu, tid, comm in server_threads[:10]))
print("generator_top_threads=" + ",".join(f"{tid}:{comm}:{cpu:.1f}%" for cpu, tid, comm in generator_threads[:10]))
print("server_max_thread_cpu=%.3f" % (server_threads[0][0] if server_threads else 0.0))
print("global_overlap_seconds=%.3f" % elapsed)
print("generator_total_cpu=%.3f generator_max_shard_cpu=%.3f generator_max_thread_cpu=%.3f" % (generator_total_cpu, generator_max_shard_cpu, generator_max_thread_cpu))
for index, item in enumerate(generators):
    print("generator_%d_cpu=%.3f" % (index, item["cpu_percent"]))
for index, values in enumerate(shard_values):
    print("generator_%d_ready=%d generator_%d_progressing=%d generator_%d_runtime_failures=%d" % (index, values["ready"], index, values["progressing"], index, values["runtime_failures"]))
print("total_allocated=%d total_ready=%d total_progressing=%d total_runtime_failures=%d total_non_progressing=%d" % (aggregate["allocated"], aggregate["ready"], aggregate["progressing"], aggregate["runtime_failures"], aggregate["non_progressing_viewers"]))
print("client_recvq_total_p95=%s client_recvq_total_max=%s client_recvq_nonzero_p95=%s client_recvq_nonzero_max=%s client_recvq_max_p95=%s client_recvq_max_max=%s" % (client_sockets["recvq_total_p95"], client_sockets["recvq_total_max"], client_sockets["recvq_nonzero_p95"], client_sockets["recvq_nonzero_max"], client_sockets["recvq_max_p95"], client_sockets["recvq_max_max"]))
PY
for generator_log in "${generator_log_paths[@]}"; do
    cat "$generator_log"
done
cat "$work_dir/results.tsv"
cat "$work_dir/thread_summary.tsv"
exit "$generator_status"
