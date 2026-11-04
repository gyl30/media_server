. /var/tmp/ms-perf-891aec1/jobs-lib-u/udp.sh
for pair in base:r1 gso:r1 gso:r2 base:r2; do
  v=${pair%:*}; r=${pair#*:}; B=build/media_server-$v
  urun rt-g $B u6-gb_udp-4000-$v-$r rtmp:16 gb_udp:4000
  urun rt-g $B u6-whep-1536-$v-$r whip:32 whep:1536
  mixrun rt-g $B u6-b-1.6-$v-$r B 1.6
  mixrun rt-g $B u6-d-2.2-$v-$r D 2.2
  mixrun rt-g $B u6-a-1.0-$v-$r A 1.0
  mixrun rt-g $B u6-c-2.15-$v-$r C 2.15
done
echo U06_DONE
