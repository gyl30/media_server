. /var/tmp/ms-perf-891aec1/jobs-lib-u/udp.sh
gtrace() {
  name=$1; shift
  P=$R/gso-$name
  rm -rf $P; mkdir -p $P
  "$@" > $P/run.txt 2>&1 &
  runner=$!
  until [ -e $R/$name/measurement.start ] || ! kill -0 $runner 2>/dev/null; do sleep 1; done
  sleep 20
  server=$(pgrep -f "threads 8 --bind-address 198.19.0.10" | head -1)
  taskset -c 15 sh $U/gso_trace.sh $server > $P/trace.txt 2>&1
  wait $runner
  cat $P/run.txt | tail -3
  cat $P/trace.txt
}
for v in gso2 base; do
  gtrace u13-gb_udp-4000-$v urun rt-g build/media_server-$v u13-gb_udp-4000-$v rtmp:16 gb_udp:4000
  gtrace u13-whep-1536-$v urun rt-g build/media_server-$v u13-whep-1536-$v whip:32 whep:1536
done
echo U13_DONE
