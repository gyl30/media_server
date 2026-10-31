T=/var/tmp/ms-perf-891aec1
NS=msperf-20261013T011210Z-7788320
R=$T/runs4
cd $T/rt-s
for san in asan tsan; do
  if [ $san = asan ]; then key=ASAN_OPTIONS; extra=":detect_leaks=1:verbosity=1"; else key=TSAN_OPTIONS; extra=":verbosity=1"; fi
  out=$R/san-control-$san
  rm -rf $out
  ip netns exec $NS python3 $T/gb_client_harness.py --gb-fanout san-$san/gb_fanout --dump $R/mx-gb-codecs/rtmp-h264-pcmu/gb_udp-0.rtp --output $out \
    --transport tcp_passive --mode malformed --fault oversized --listen-port 21000 --env $key=log_path=$out/sanitizer$extra | cut -c1-300
  env LD_LIBRARY_PATH=lib $key=log_path=$out/sanitizer-demux$extra san-$san/gb_ps_demux --input $R/mx-gb-codecs/rtmp-h264-pcmu/gb_udp-0.rtp \
    --output-prefix $out/demux | cut -c1-120
  for f in $out/sanitizer*; do echo "$(basename $f): $(head -c 160 $f | tr '\n' ' ')"; done
  nm -D san-$san/gb_fanout 2>/dev/null | grep -c "__asan_init\|__tsan_init"; strings san-$san/gb_fanout | grep -m1 -o "AddressSanitizer\|ThreadSanitizer"
done
echo SAN_CONTROL_DONE
