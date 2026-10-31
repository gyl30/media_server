. /var/tmp/ms-perf-891aec1/lib3.sh
CHROME="minptime=10;useinbandfec=1"
ALL_C=whep:64,rtmp:64,flv:64,rtsp:64,hls:16,gb_udp:32,gb_tcp_active:16,gb_tcp_passive:16
for rep in 1 2; do
  for variant in a b; do
    bin=$([ $variant = a ] && echo build/media_server || echo build-b/media_server)
    export SERVER_BIN=$bin
    traced ab-none-$variant-$rep --input whip --sources 64 --outputs none $C
    traced ab-whep-$variant-$rep --input whip --sources 64 --outputs whep:128 $C
    traced ab-whepchrome-$variant-$rep --input whip --sources 64 --outputs whep:128 --whep-opus-fmtp "$CHROME" $C
    traced ab-flv-$variant-$rep --input whip --sources 64 --outputs flv:128 $C
    traced ab-all-$variant-$rep --input whip --sources 64 --outputs $ALL_C $C
    traced ab-pull-whep-$variant-$rep --input whep_pull --sources 32 --outputs whep:64 --upstream-workers 4 --upstream-bin build/media_server $C
    unset SERVER_BIN
  done
done
echo AB_DONE
