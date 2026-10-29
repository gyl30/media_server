T=/var/tmp/ms-perf-891aec1
X=$T/fixtures-a1/avsync_720p30.flv
U="--upstream-workers 16 --fixture $X --av-probe 40 --decode-probe --warmup 15 --duration 60 --ready-timeout 300 --ramp-per-second 200 --group-size 256 --cleanup-settle 5 --min-free-disk-mib 3584"
run() { rt=$1; name=$2; shift 2; RT=$rt $T/pr2.sh $name "$@"; gzip $T/runs/$name/result.json; }
run rt-ls av-L-1 --input whep_pull --sources 1 --outputs rtmp:2,flv:2,rtsp:2,hls:1 $U
run rt-fdk av-F-1 --input whep_pull --sources 1 --outputs rtmp:2,flv:2,rtsp:2,hls:1 $U
run rt-ls av-L-mix150 --input whep_pull --sources 150 --outputs flv:147,rtmp:1,rtsp:1,hls:1 --perf-stat --pull-interleave-seed 11 $U
run rt-fdk av-F-mix250 --input whep_pull --sources 250 --outputs flv:247,rtmp:1,rtsp:1,hls:1 --perf-stat --pull-interleave-seed 11 $U
