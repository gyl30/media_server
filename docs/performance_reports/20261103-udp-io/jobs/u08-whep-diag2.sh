. /var/tmp/ms-perf-891aec1/jobs-lib-u/udp.sh
for v in gso2 base; do
  uprof u8-whep-1536-$v urun rt-g build/media_server-$v u8-whep-1536-$v whip:32 whep:1536
done
for pair in base:r1 gso2:r1; do
  v=${pair%:*}; r=${pair#*:}
  urun rt-g build/media_server-$v u8-whep-1536-$v-$r whip:32 whep:1536
  mixrun rt-g build/media_server-$v u8-b-1.6-$v-$r B 1.6
done
echo U08_DONE
