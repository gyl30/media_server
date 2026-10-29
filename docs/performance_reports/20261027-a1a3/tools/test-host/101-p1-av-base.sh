T=/var/tmp/ms-perf-891aec1
O="--sources 1 --outputs rtmp:2,flv:2,rtsp:2,hls:1,whep:2 --av-probe 40 --warmup 10 --duration 45 --cleanup-settle 5"
RT=rt-p1-base $T/pr2.sh p1-av-rtmp-base-r1 --input rtmp --fixture /var/tmp/ms-perf-891aec1/fixtures-a1/avsync_720p30.flv $O
RT=rt-p1-base $T/pr2.sh p1-av-whep_pull-base-r1 --input whep_pull --fixture /var/tmp/ms-perf-891aec1/fixtures-a1/avsync_720p30.flv $O
RT=rt-p1-base $T/pr2.sh p1-av-whip-base-r1 --input whip --whip-fixture /var/tmp/ms-perf-891aec1/fixtures-a1/avsync_whip.mkv $O
RT=rt-p1-base $T/pr2.sh p1-av-rtsp_tcp-base-r1 --input rtsp_tcp --fixture /var/tmp/ms-perf-891aec1/fixtures-a1/avsync_720p30.flv $O
RT=rt-p1-base $T/pr2.sh p1-av-rtsp_tcp_bf-base-r1 --input rtsp_tcp --fixture /var/tmp/ms-perf-891aec1/fixtures-a1/avsync_bframes.flv $O
RT=rt-p1-base $T/pr2.sh p1-av-rtmp_bf-base-r1 --input rtmp --fixture /var/tmp/ms-perf-891aec1/fixtures-a1/avsync_bframes.flv $O
