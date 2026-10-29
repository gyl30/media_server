T=/var/tmp/ms-perf-891aec1
X=/var/tmp/ms-perf-891aec1/fixtures-a1
O="--sources 1 --outputs rtmp:2,flv:2,rtsp:2,hls:1,whep:2 --av-probe 40 --warmup 10 --duration 45 --cleanup-settle 5"
OB="--sources 1 --outputs rtmp:2,flv:2,rtsp:2,hls:1 --av-probe 40 --warmup 10 --duration 45 --cleanup-settle 5"
for i in 2 3 4 5; do
RT=rt-p1-f1 $T/pr2.sh p1-av-whep_pull-f1-r$((i-1)) --input whep_pull --fixture $X/avsync_720p30.flv $O
RT=rt-p1-base $T/pr2.sh p1-av-whep_pull-base-r$i --input whep_pull --fixture $X/avsync_720p30.flv $O
done
RT=rt-p1-f1 $T/pr2.sh p1-av-whip-f1-r1 --input whip --whip-fixture $X/avsync_whip.mkv $O
RT=rt-p1-f1 $T/pr2.sh p1-av-rtsp_tcp-f1-r1 --input rtsp_tcp --fixture $X/avsync_720p30.flv $O
RT=rt-p1-base $T/pr2.sh p1-av-rtsp_tcp-base-r2 --input rtsp_tcp --fixture $X/avsync_720p30.flv $O
RT=rt-p1-f1 $T/pr2.sh p1-av-rtsp_tcp-f1-r2 --input rtsp_tcp --fixture $X/avsync_720p30.flv $O
RT=rt-p1-base $T/pr2.sh p1-av-rtsp_bf-base-r1 --input rtsp_tcp --fixture $X/avsync_bframes.flv $OB
RT=rt-p1-f1 $T/pr2.sh p1-av-rtsp_bf-f1-r1 --input rtsp_tcp --fixture $X/avsync_bframes.flv $OB
RT=rt-p1-base $T/pr2.sh p1-av-rtmp_bf-base-r2 --input rtmp --fixture $X/avsync_bframes.flv $OB
RT=rt-p1-f1 $T/pr2.sh p1-av-rtsp_udp-f1-r1 --input rtsp_udp --fixture $X/avsync_720p30.flv $O
RT=rt-p1-base $T/pr2.sh p1-av-rtsp_udp-base-r1 --input rtsp_udp --fixture $X/avsync_720p30.flv $O
