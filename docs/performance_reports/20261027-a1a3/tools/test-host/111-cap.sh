T=/var/tmp/ms-perf-891aec1
S="--perf-stat --control-probe-ms 200 --input rtmp --sources 1 --warmup 15 --ready-timeout 300 --ramp-per-second 200 --group-size 256"
G="--gb-port-base 18700 $S"
M="rtmp:1250,flv:1250,rtsp:1250,whep:1250,gb_udp:1250,gb_tcp_passive:1250"
for n in 12000 13000; do
RT=rt-a11gdf $T/pr2.sh v3-gbp$n-gd-r1 --outputs gb_tcp_passive:$n --duration 60 $G
RT=rt-cfe80a0 $T/pr2.sh v3-gbp$n-a2-r1 --outputs gb_tcp_passive:$n --duration 60 $G
RT=rt-a2A-c999003 $T/pr2.sh v3-gbp$n-pre-r1 --outputs gb_tcp_passive:$n --duration 60 $G
done
RT=rt-a11gdf $T/pr2.sh cap-rtmp9500-F-r1 --outputs rtmp:9500 --duration 60 $S
RT=rt-a11gdf $T/pr2.sh cap-rtsp9000-F-r1 --outputs rtsp:9000 --duration 60 $S
RT=rt-a11gdf $T/pr2.sh cap-flv9000-F-r1 --outputs flv:9000 --duration 60 $S
RT=rt-a11gdf $T/pr2.sh cap-mix7500-F-r1 --outputs $M --duration 60 $S
RT=rt-cfe80a0 $T/pr2.sh cap-mix7500-A-r1 --outputs $M --duration 60 $S
RT=rt-a11gdf $T/pr2.sh cap-rtmp10000-F-r1 --outputs rtmp:10000 --duration 60 $S
RT=rt-a11gdf $T/pr2.sh cap-rtsp9500-F-r1 --outputs rtsp:9500 --duration 60 $S
RT=rt-a11gdf $T/pr2.sh cap-flv9500-F-r1 --outputs flv:9500 --duration 60 $S
RT=rt-a11gdf $T/pr2.sh cap-rtmp9500-F-r2 --outputs rtmp:9500 --duration 60 $S
RT=rt-a11gdf $T/pr2.sh cap-rtsp9000-F-r2 --outputs rtsp:9000 --duration 60 $S
RT=rt-a11gdf $T/pr2.sh cap-flv9000-F-r2 --outputs flv:9000 --duration 60 $S
RT=rt-a11gdf $T/pr2.sh cap-mix7500-F-r2 --outputs $M --duration 60 $S
RT=rt-a11gdf $T/pr2.sh cap-rtmp9000s100-F-r1 --outputs rtmp:9000 --duration 60 --perf-stat --control-probe-ms 200 --input rtmp --sources 100 --warmup 15 --ready-timeout 300 --ramp-per-second 200 --group-size 256
RT=rt-a11gdf $T/pr2.sh cap-whep3500-F-r1 --outputs whep:3500 --churn whep:50 --duration 60 $S
RT=rt-cfe80a0 $T/pr2.sh cap-whep3500-A-r1 --outputs whep:3500 --churn whep:50 --duration 60 $S
RT=rt-a11gdf $T/pr2.sh cap-gbudp7500-F-r1 --outputs gb_udp:7500 --duration 60 $S
RT=rt-cfe80a0 $T/pr2.sh cap-gbudp7500-A-r1 --outputs gb_udp:7500 --duration 60 $S
