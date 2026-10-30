. /var/tmp/ms-perf-891aec1/lib.sh
cp $T/stage/perf_run.py $T/stage/whep_dump_av.py $T/rt-q/bench/
cp $T/stage/whep_fanout $T/stage/whip_publisher $T/rt-q/build/
sha256sum $T/rt-q/build/* $T/rt-q/bench/*.py > $R/rt-q.sha256
declare -A B1=([whep]=200 [rtmp]=500 [flv]=500 [rtsp]=500 [hls]=100 [gb_udp]=200 [gb_tcp_active]=100 [gb_tcp_passive]=100)
for x in whep rtmp flv rtsp hls gb_udp gb_tcp_active gb_tcp_passive; do
  run w-$x-a --input whip --sources 1 --outputs $x:4 --av-probe 20 --decode-probe $C
  run w-$x-b1 --input whip --sources 4 --outputs $x:${B1[$x]} $C
  run w-$x-b2 --input whip --sources 4 --outputs $x:$((2 * ${B1[$x]})) $C
  run w-$x-c1 --input whip --sources 32 --outputs $x:64 $C
  run w-$x-c2 --input whip --sources 64 --outputs $x:128 $C
done
CHROME="minptime=10;useinbandfec=1"
run w-whepchrome-b1 --input whip --sources 4 --outputs whep:200 --whep-opus-fmtp "$CHROME" $C
run w-whepchrome-b2 --input whip --sources 4 --outputs whep:400 --whep-opus-fmtp "$CHROME" $C
run w-whepchrome-c1 --input whip --sources 32 --outputs whep:64 --whep-opus-fmtp "$CHROME" $C
run w-whepchrome-c2 --input whip --sources 64 --outputs whep:128 --whep-opus-fmtp "$CHROME" $C
ALL_A=rtmp:4,rtsp:4,flv:4,hls:2,whep:4,gb_udp:2,gb_tcp_active:2,gb_tcp_passive:2
ALL_B=whep:100,rtmp:150,flv:150,rtsp:150,hls:25,gb_udp:50,gb_tcp_active:25,gb_tcp_passive:25
ALL_C=whep:64,rtmp:64,flv:64,rtsp:64,hls:16,gb_udp:32,gb_tcp_active:16,gb_tcp_passive:16
RT=rt-q traced w-all-a --input whip --sources 1 --outputs $ALL_A --av-probe 20 --decode-probe $C
run w-all-b --input whip --sources 4 --outputs $ALL_B $C
traced w-all-c --input whip --sources 64 --outputs $ALL_C $C
traced w-allchrome-c --input whip --sources 64 --outputs $ALL_C --whep-opus-fmtp "$CHROME" $C
run w-none-c1 --input whip --sources 32 --outputs none $C
run w-none-c2 --input whip --sources 64 --outputs none $C
echo WSCAN_DONE
