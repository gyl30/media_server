. /var/tmp/ms-perf-891aec1/lib3.sh
CHROME="minptime=10;useinbandfec=1"
ALL_A=rtmp:4,rtsp:4,flv:4,hls:2,whep:4,gb_udp:2,gb_tcp_active:2,gb_tcp_passive:2
for variant in a b; do
  export SERVER_BIN=$([ $variant = a ] && echo build/media_server || echo build-b/media_server)
  for x in whep rtmp flv rtsp hls gb_udp gb_tcp_active gb_tcp_passive; do
    run av-$x-$variant --input whip --sources 1 --outputs $x:4 --av-probe 20 --decode-probe $C
  done
  run av-whepchrome-$variant --input whip --sources 1 --outputs whep:4 --whep-opus-fmtp "$CHROME" --av-probe 20 $C
  run av-all-$variant --input whip --sources 1 --outputs $ALL_A --av-probe 20 --decode-probe $C
  run av-pull-all-$variant --input whep_pull --upstream-bin build/media_server --sources 1 --outputs $ALL_A --av-probe 20 --decode-probe $C
  unset SERVER_BIN
done
echo AV_DONE
