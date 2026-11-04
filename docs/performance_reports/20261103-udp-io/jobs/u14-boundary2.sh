. /var/tmp/ms-perf-891aec1/jobs-lib-u/udp.sh
for r in r1 r2; do
  mixrun rt-g build/media_server-gso2 u14-d-2.6-gso2-$r D 2.6
  for v in base gso2; do
    mixrun rt-g build/media_server-$v u14-b-1.7-$v-$r B 1.7
  done
done
for v in base gso2; do
  urun rt-g build/media_server-$v u14-gb_udp-10000-$v-r1 rtmp:40 gb_udp:10000
  urun rt-g build/media_server-$v u14-whep-2560-$v-r1 whip:40 whep:2560
done
echo U14_DONE
