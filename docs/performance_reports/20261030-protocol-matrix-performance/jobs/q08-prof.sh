. /var/tmp/ms-perf-891aec1/lib.sh
P=$T/profile_run.sh
CHROME="minptime=10;useinbandfec=1"
$P p-whep-c2 --input whip --sources 64 --outputs whep:128 $C
$P p-whepchrome-c2 --input whip --sources 64 --outputs whep:128 --whep-opus-fmtp "$CHROME" $C
$P p-flv-c2 --input whip --sources 64 --outputs flv:128 $C
$P p-whep-b2 --input whip --sources 4 --outputs whep:400 $C
$P p-flv-b2 --input whip --sources 4 --outputs flv:1000 $C
$P p-rtmp-b2 --input whip --sources 4 --outputs rtmp:1000 $C
$P p-rtsp-b2 --input whip --sources 4 --outputs rtsp:1000 $C
$P p-hls-b2 --input whip --sources 4 --outputs hls:200 $C
$P p-gbudp-b2 --input whip --sources 4 --outputs gb_udp:400 $C
$P p-gbtcpa-b2 --input whip --sources 4 --outputs gb_tcp_active:200 $C
$P p-all-c --input whip --sources 64 --outputs whep:64,rtmp:64,flv:64,rtsp:64,hls:16,gb_udp:32,gb_tcp_active:16,gb_tcp_passive:16 $C
$P p-rtmpaac-flv-c2 --input rtmp --sources 64 --outputs flv:128 $C
echo PROF_DONE
