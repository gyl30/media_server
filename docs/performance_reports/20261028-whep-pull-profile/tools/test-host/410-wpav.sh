T=/var/tmp/ms-perf-891aec1
X=$T/fixtures-a1/avsync_720p30.flv
U="--upstream-workers 16 --fixture $X --av-probe 40 --decode-probe --warmup 15 --duration 60 --ready-timeout 300 --ramp-per-second 200 --group-size 256 --cleanup-settle 5"
run() { name=$1; shift; RT=rt-wp $T/pr2.sh $name "$@"; gzip $T/runs/$name/result.json; }
run wq-av1-r1b --input whep_pull --sources 1 --outputs rtmp:2,flv:2,rtsp:2,hls:1 $U
run wq-av150-seq-r1b --input whep_pull --sources 150 --outputs flv:147,rtmp:1,rtsp:1,hls:1 --perf-stat $U
run wq-av150-mix11-r1b --input whep_pull --sources 150 --outputs flv:147,rtmp:1,rtsp:1,hls:1 --perf-stat --pull-interleave-seed 11 $U
run wq-av1-r2b --input whep_pull --sources 1 --outputs rtmp:2,flv:2,rtsp:2,hls:1 $U
