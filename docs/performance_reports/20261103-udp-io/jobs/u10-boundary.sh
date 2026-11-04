. /var/tmp/ms-perf-891aec1/jobs-lib-u/udp.sh
for r in r1 r2; do
  for v in base gso2; do
    B=build/media_server-$v
    urun rt-g $B u10-gb_udp-7000-$v-$r rtmp:28 gb_udp:7000
    urun rt-g $B u10-whep-2048-$v-$r whip:32 whep:2048
    mixrun rt-g $B u10-b-1.8-$v-$r B 1.8
    mixrun rt-g $B u10-d-2.4-$v-$r D 2.4
  done
done
echo U10_DONE
