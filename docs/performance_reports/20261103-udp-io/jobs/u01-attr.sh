. /var/tmp/ms-perf-891aec1/jobs-lib-u/udp.sh
B=build/media_server
uprof u1-gb_udp-4000 urun rt-g $B u1-gb_udp-4000 rtmp:16 gb_udp:4000
uprof u1-whep-1536 urun rt-g $B u1-whep-1536 whip:32 whep:1536
uprof u1-in-whip-160 urun rt-g $B u1-in-whip-160 whip:160 whep:160
uprof u1-d-2.2 mixrun rt-g $B u1-d-2.2 D 2.2
uprof u1-b-1.6 mixrun rt-g $B u1-b-1.6 B 1.6
uprof u1-c-2.15 mixrun rt-g $B u1-c-2.15 C 2.15
uprof u1-a-1.0 mixrun rt-g $B u1-a-1.0 A 1.0
echo U01_DONE
