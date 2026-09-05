#!/usr/bin/env bash
set -euo pipefail

server_bin="${1:-./build/media_server}"
work_dir="${2:-./network_test_output}"
mkdir -p "$work_dir"
work_dir="$(cd "$work_dir" && pwd)"
server_bin="$(realpath "$server_bin")"

ffprobe_version="$(ffprobe -version | head -1)"
ffprobe_major="$(sed -n 's/^ffprobe version n\?\([0-9][0-9]*\).*/\1/p' <<<"$ffprobe_version")"
if [[ -z "$ffprobe_major" || "$ffprobe_major" -lt 8 ]]; then
    echo "ffprobe 8 or newer is required for AV1 RTP integration tests: $ffprobe_version" >&2
    exit 1
fi

main_pid=""
pull_pid=""
publish_pid=""
rtsp_publish_pid=""
av1_server_pid=""
av1_publish_pid=""
cleanup() {
    set +e
    [[ -n "$av1_publish_pid" ]] && kill "$av1_publish_pid" 2>/dev/null
    [[ -n "$av1_server_pid" ]] && kill "$av1_server_pid" 2>/dev/null
    [[ -n "$rtsp_publish_pid" ]] && kill "$rtsp_publish_pid" 2>/dev/null
    [[ -n "$publish_pid" ]] && kill "$publish_pid" 2>/dev/null
    [[ -n "$pull_pid" ]] && kill "$pull_pid" 2>/dev/null
    [[ -n "$main_pid" ]] && kill "$main_pid" 2>/dev/null
    [[ -n "$av1_publish_pid" ]] && wait "$av1_publish_pid" 2>/dev/null
    [[ -n "$av1_server_pid" ]] && wait "$av1_server_pid" 2>/dev/null
    [[ -n "$rtsp_publish_pid" ]] && wait "$rtsp_publish_pid" 2>/dev/null
    [[ -n "$publish_pid" ]] && wait "$publish_pid" 2>/dev/null
    [[ -n "$pull_pid" ]] && wait "$pull_pid" 2>/dev/null
    [[ -n "$main_pid" ]] && wait "$main_pid" 2>/dev/null
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

wait_log "$work_dir/server.log" 'rtmp publish live/test'
wait_log "$work_dir/server.log" 'rtmp input tracks ready audio true'
sleep 1

probe_streams "$work_dir/rtsp_from_rtmp.txt" -rtsp_transport tcp 'rtsp://127.0.0.1:18554/live/test'
probe_streams "$work_dir/rtmp_from_rtmp.txt" 'rtmp://127.0.0.1:19350/live/test'
probe_streams "$work_dir/http_flv_from_rtmp.txt" 'http://127.0.0.1:18080/live/test.flv'

# 首次请求建立共享 HLS 输出；等待自然关键帧完成切片。
probe_hls_ts hls_from_rtmp 'http://127.0.0.1:18080/play/hls/live/test'

"$server_bin" --rtmp-port 19351 --rtsp-port 18555 --http-port 18081 \
    --rtsp-pull 'relay/test=rtsp://127.0.0.1:18554/live/test' \
    >"$work_dir/pull_server.log" 2>&1 &
pull_pid=$!

wait_log "$work_dir/pull_server.log" 'rtsp input connected stream relay/test'
wait_log "$work_dir/pull_server.log" 'rtsp input tracks ready audio true'
sleep 1

probe_streams "$work_dir/rtsp_from_rtsp.txt" -rtsp_transport tcp 'rtsp://127.0.0.1:18555/relay/test'
probe_streams "$work_dir/rtmp_from_rtsp.txt" 'rtmp://127.0.0.1:19351/relay/test'
probe_streams "$work_dir/http_flv_from_rtsp.txt" 'http://127.0.0.1:18081/relay/test.flv'

probe_hls_ts hls_from_rtsp 'http://127.0.0.1:18081/play/hls/relay/test'

# RTSP push TCP/UDP 使用独立 stream；UDP 连续建立两次，覆盖传输资源释放后的再次建链。
for push_case in tcp udp udp-restart; do
    transport="${push_case%%-*}"
    stream_name="rtsp-push-$push_case"
    ffmpeg -nostdin -hide_banner -loglevel error -re \
        -f lavfi -i 'testsrc=size=320x180:rate=25' \
        -f lavfi -i 'sine=frequency=1200:sample_rate=44100' \
        -map 0:v:0 -map 1:a:0 \
        -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p \
        -g 25 -keyint_min 25 -sc_threshold 0 \
        -c:a aac -b:a 96k -ac 2 \
        -t 24 -rtsp_transport "$transport" -f rtsp "rtsp://127.0.0.1:18554/live/$stream_name" \
        >"$work_dir/rtsp_publish_${push_case}.log" 2>&1 &
    rtsp_publish_pid=$!

    wait_probe_streams "$work_dir/rtsp_push_${push_case}_rtsp.txt" h264 aac -rtsp_transport tcp \
        "rtsp://127.0.0.1:18554/live/$stream_name"
    probe_streams "$work_dir/rtsp_push_${push_case}_rtmp.txt" "rtmp://127.0.0.1:19350/live/$stream_name"
    probe_streams "$work_dir/rtsp_push_${push_case}_http_flv.txt" "http://127.0.0.1:18080/live/$stream_name.flv"
    probe_hls_ts "rtsp_push_${push_case}_hls" "http://127.0.0.1:18080/play/hls/live/$stream_name"

    kill "$rtsp_publish_pid" 2>/dev/null || true
    wait "$rtsp_publish_pid" 2>/dev/null || true
    rtsp_publish_pid=""
    sleep 0.2
    kill -0 "$main_pid"
done

# RTSP pull AV1 回归使用独立 H.264 RTSP 源，避免依赖前面已经结束的 RTMP publisher。
ffmpeg -nostdin -hide_banner -loglevel error -re \
    -f lavfi -i 'testsrc=size=320x180:rate=25' \
    -f lavfi -i 'sine=frequency=1300:sample_rate=44100' \
    -map 0:v:0 -map 1:a:0 \
    -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p \
    -g 25 -keyint_min 25 -sc_threshold 0 \
    -c:a aac -b:a 96k -ac 2 \
    -rtsp_transport tcp -f rtsp 'rtsp://127.0.0.1:18554/live/av1-pull-source' \
    >"$work_dir/av1_rtsp_pull_source.log" 2>&1 &
rtsp_publish_pid=$!

wait_probe_streams "$work_dir/av1_rtsp_pull_source.txt" h264 aac -rtsp_transport tcp \
    'rtsp://127.0.0.1:18554/live/av1-pull-source'

# AV1 作为显式输出能力启用：RTMP/HTTP-FLV 使用 Enhanced FLV，HLS 使用 fMP4，RTSP 使用 AV1/RTP。
"$server_bin" --rtmp-port 19352 --rtsp-port 18556 --http-port 18082 \
    --rtmp-video-codec av1 --rtsp-video-codec av1 --http-video-codec av1 \
    --rtsp-pull 'relay/av1=rtsp://127.0.0.1:18554/live/av1-pull-source' \
    >"$work_dir/av1_server.log" 2>&1 &
av1_server_pid=$!
sleep 0.4

ffmpeg -nostdin -hide_banner -loglevel error -re \
    -f lavfi -i 'testsrc=size=320x180:rate=25' \
    -f lavfi -i 'sine=frequency=1400:sample_rate=44100' \
    -map 0:v:0 -map 1:a:0 \
    -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p \
    -g 25 -keyint_min 25 -sc_threshold 0 \
    -c:a aac -b:a 96k -ac 2 \
    -f flv 'rtmp://127.0.0.1:19352/live/av1' \
    >"$work_dir/av1_publisher.log" 2>&1 &
av1_publish_pid=$!

wait_log "$work_dir/av1_server.log" 'rtmp publish live/av1'
wait_log "$work_dir/av1_server.log" 'rtsp input connected stream relay/av1'
wait_log "$work_dir/av1_server.log" 'rtsp input tracks ready audio true'

wait_probe_streams "$work_dir/rtsp_av1_from_rtmp.txt" av1 aac -rtsp_transport tcp \
    'rtsp://127.0.0.1:18556/live/av1'
wait_probe_streams "$work_dir/rtsp_av1_from_pull.txt" av1 aac -rtsp_transport tcp \
    'rtsp://127.0.0.1:18556/relay/av1'

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

# RTSP push TCP/UDP 继续以 H.264 输入，验证同一 RTSP 服务的 AV1 playback 输出。
for transport in tcp udp; do
    stream_name="rtsp-av1-$transport"
    ffmpeg -nostdin -hide_banner -loglevel error -re \
        -f lavfi -i 'testsrc=size=320x180:rate=25' \
        -f lavfi -i 'sine=frequency=1500:sample_rate=44100' \
        -map 0:v:0 -map 1:a:0 \
        -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p \
        -g 25 -keyint_min 25 -sc_threshold 0 \
        -c:a aac -b:a 96k -ac 2 \
        -rtsp_transport "$transport" -f rtsp "rtsp://127.0.0.1:18556/live/$stream_name" \
        >"$work_dir/rtsp_av1_publish_${transport}.log" 2>&1 &
    rtsp_publish_pid=$!

    wait_probe_streams "$work_dir/rtsp_av1_push_${transport}.txt" av1 aac -rtsp_transport tcp \
        "rtsp://127.0.0.1:18556/live/$stream_name"

    kill "$rtsp_publish_pid" 2>/dev/null || true
    wait "$rtsp_publish_pid" 2>/dev/null || true
    rtsp_publish_pid=""
    sleep 0.2
    kill -0 "$av1_server_pid"
done

kill -0 "$main_pid"
kill -0 "$pull_pid"
kill -0 "$av1_server_pid"

if grep -Fq 'rtsp av1 transcode failed' "$work_dir/av1_server.log"; then
    echo 'rtsp av1 transcode failure detected' >&2
    grep -F 'rtsp av1 transcode failed' "$work_dir/av1_server.log" >&2
    exit 1
fi

cat >"$work_dir/summary.txt" <<SUMMARY
rtmp input -> rtsp output: pass
rtmp input -> rtmp output: pass
rtmp input -> http-flv output: pass
rtmp input -> hls output: pass
rtsp input -> rtsp output: pass
rtsp input -> rtmp output: pass
rtsp input -> http-flv output: pass
rtsp input -> hls output: pass
rtsp push tcp -> rtsp/rtmp/http-flv/hls outputs: pass
rtsp push udp -> rtsp/rtmp/http-flv/hls outputs: pass
rtsp push udp restart -> rtsp/rtmp/http-flv/hls outputs: pass
rtmp input -> rtsp av1 output: pass
rtsp pull input -> rtsp av1 output: pass
rtsp push tcp -> rtsp av1 output: pass
rtsp push udp -> rtsp av1 output: pass
rtsp av1 client churn: pass
rtmp explicit av1 output: pass
rtmp av1 rejects peer without av01: pass
http-flv explicit av1 output: pass
hls explicit av1 fmp4 output: pass
all servers remained alive after client disconnects: pass
SUMMARY
cat "$work_dir/summary.txt"
