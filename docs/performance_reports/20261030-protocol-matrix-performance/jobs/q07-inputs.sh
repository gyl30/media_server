. /var/tmp/ms-perf-891aec1/lib.sh
F=$R/mx-rel/fixtures
ALL=rtmp:32,rtsp:32,flv:32,hls:8,whep:32,gb_udp:16,gb_tcp_active:8,gb_tcp_passive:8
NOWHEP=rtmp:32,rtsp:32,flv:32,hls:8,gb_udp:16,gb_tcp_active:8,gb_tcp_passive:8
NOGB=rtmp:32,rtsp:32,flv:32,hls:8,whep:32
UP="--upstream-workers 16 --upstream-sources 16 --upstream-bin $T/rt-e1/upstream.sh"
OPUS="--ffmpeg-bin ffmpeg901/bin/ffmpeg --ffprobe-bin ffmpeg901/bin/ffprobe"
pair() { name=$1; outs=$2; shift 2; run i-$name-none "$@" --sources 16 --outputs none $C; run i-$name-all "$@" --sources 16 --outputs $outs $C; }
pair rtmp-aac $ALL --input rtmp --fixture $F/avsync_720p30.flv
pair rtmp-opus $NOGB --input rtmp --fixture $F/h264_opus.flv $OPUS
pair rtmp-pcma $ALL --input rtmp --fixture $F/h264_pcma.flv
pair rtmp-h265 $NOWHEP --input rtmp --fixture $F/h265_aac.flv
pair rtsptcp-aac $ALL --input rtsp_tcp --fixture $F/avsync_720p30.flv
pair rtspudp-aac $ALL --input rtsp_udp --fixture $F/avsync_720p30.flv
pair rtsptcp-opus $NOGB --input rtsp_tcp --fixture $F/avsync_whip.mkv $OPUS
pair rtsptcp-pcma $ALL --input rtsp_tcp --fixture $F/h264_pcma.flv
pair whip-opus $ALL --input whip
pair rtmppull-aac $ALL --input rtmp_pull $UP
pair rtsppull-aac $ALL --input rtsp_pull $UP
pair wheppull-aac $ALL --input whep_pull $UP
pair gbudp-aac $ALL --input gb_udp $UP
pair gbtcpa-aac $ALL --input gb_tcp_active $UP
pair gbtcpp-aac $ALL --input gb_tcp_passive $UP
echo INPUTS_DONE
