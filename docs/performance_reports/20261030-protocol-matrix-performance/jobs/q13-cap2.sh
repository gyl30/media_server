. /var/tmp/ms-perf-891aec1/lib.sh
CHROME="minptime=10;useinbandfec=1"
st() { python3 $T/st.py $R/$1 | cut -d" " -f1; }
run cap-whep-384-r2 --input whip --sources 384 --outputs whep:768 --group-size 128 $C
run cap-whepchrome-288-r1 --input whip --sources 288 --outputs whep:576 --group-size 128 --whep-opus-fmtp "$CHROME" $C
[ "$(st cap-whepchrome-288-r1)" = PASS ] && run cap-whepchrome-288-r2 --input whip --sources 288 --outputs whep:576 --group-size 128 --whep-opus-fmtp "$CHROME" $C
echo CAP2_DONE
for x in rtmp flv rtsp gb_udp gb_tcp_active gb_tcp_passive; do
  run cap-$x-416-r1 --input whip --sources 416 --outputs $x:832 $C
done
run cap-hls-480-r1 --input whip --sources 480 --outputs hls:960 $C
run cap-whep-352-r1 --input whip --sources 352 --outputs whep:704 --group-size 128 $C
run cap-whepchrome-224-r1 --input whip --sources 224 --outputs whep:448 --group-size 128 --whep-opus-fmtp "$CHROME" $C
run cap-all-288-r1 --input whip --sources 288 --outputs whep:288,rtmp:288,flv:288,rtsp:288,hls:72,gb_udp:144,gb_tcp_active:72,gb_tcp_passive:72 --group-size 128 $C
echo CAP3_DONE
