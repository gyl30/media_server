. /var/tmp/ms-perf-891aec1/jobs-lib-l/l.sh
for pair in lib:r1 base:r1 lib:r2 base:r2; do
  v=${pair%:*}; r=${pair#*:}
  B=$([ $v = lib ] && echo build/media_server || echo $T/rt-g/build/media_server-base)
  urun rt-l $B l3-gb_udp-4000-$v-$r rtmp:16 gb_udp:4000
  urun rt-l $B l3-whep-1536-$v-$r whip:32 whep:1536
  mixrun rt-l $B l3-d-2.2-$v-$r D 2.2
done
echo L03_DONE
