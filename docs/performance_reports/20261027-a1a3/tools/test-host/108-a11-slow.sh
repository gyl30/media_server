while ps -eo args | grep -q "[p]r2.sh a3-gbudp300-S-r1"; do sleep 10; done
T=/var/tmp/ms-perf-891aec1
RT=rt-cfe80a0 $T/pr2.sh a11-slow120-A-r1 --input rtmp --sources 1 --outputs rtmp:1000,rtsp:1000,gb_tcp_passive:500 --stalled-viewers 20 --control-probe-ms 200 --warmup 10 --duration 120 --cleanup-settle 10
RT=rt-a11gdf $T/pr2.sh a11-slow120-GD-r1 --input rtmp --sources 1 --outputs rtmp:1000,rtsp:1000,gb_tcp_passive:500 --stalled-viewers 20 --control-probe-ms 200 --warmup 10 --duration 120 --cleanup-settle 10
