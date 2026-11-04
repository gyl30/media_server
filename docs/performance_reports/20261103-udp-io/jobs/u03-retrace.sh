. /var/tmp/ms-perf-891aec1/jobs-lib-u/udp.sh
B=build/media_server
utrace u3-gb_udp-4000 urun rt-g $B u3-gb_udp-4000 rtmp:16 gb_udp:4000
utrace u3-whep-1536 urun rt-g $B u3-whep-1536 whip:32 whep:1536
echo U03_DONE
