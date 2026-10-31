. /var/tmp/ms-perf-891aec1/lib4.sh
ALL_C=whep:64,rtmp:64,flv:64,rtsp:64,hls:16,gb_udp:32,gb_tcp_active:16,gb_tcp_passive:16
for rep in 1 2; do
  for variant in a d; do
    export SERVER_BIN=$([ $variant = a ] && echo build/media_server || echo build-d/media_server)
    traced ab-chrome-$variant-$rep --input whip --sources 64 --outputs whep:128 --whep-opus-fmtp "$CHROME" $C
    traced ab-chromeflv-$variant-$rep --input whip --sources 64 --outputs whep:128,flv:128 --whep-opus-fmtp "$CHROME" $C
    traced ab-chromeall-$variant-$rep --input whip --sources 64 --outputs $ALL_C --whep-opus-fmtp "$CHROME" $C
    unset SERVER_BIN
  done
done
echo AB_DONE
search a build/media_server 256 32 512
search d build-d/media_server 768 64 1216
echo CAP_DONE
