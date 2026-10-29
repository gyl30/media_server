T=/var/tmp/ms-perf-891aec1
RT=rt-a2B3s20-c999003 $T/pr2.sh p1-f1-whep_pull-base-r1 --input whep_pull --sources 1 --outputs rtmp:4,flv:4,rtsp:4,whep:4,gb_udp:4,hls:2 --decode-probe --warmup 10 --duration 60 --cleanup-settle 5
RT=rt-a2B3s20-c999003 $T/pr2.sh p1-f1-whip-base-r1 --input whip --sources 1 --outputs rtmp:4,flv:4,rtsp:4,whep:4,gb_udp:4,hls:2 --decode-probe --warmup 10 --duration 60 --cleanup-settle 5
