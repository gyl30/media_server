T=/var/tmp/ms-perf-891aec1
R=$T/runs4
cd $T/rt-s
for san in asan tsan; do
  if [ $san = asan ]; then key=ASAN_OPTIONS; extra=":detect_leaks=1"; else key=TSAN_OPTIONS; extra=""; fi
  for transport in udp tcp_active tcp_passive; do
    out=$R/san-gb-$san/$transport-valid
    env LD_LIBRARY_PATH=lib $key=log_path=$out/sanitizer-demux$extra python3 bench/gb_av_probe.py $out.rtp --demux-bin san-$san/gb_ps_demux \
      --ffmpeg-bin bin/ffmpeg --expect-audio > $out/probe.json
    python3 -c "import json; r=json.load(open('$out/probe.json')); print('$san $transport dump-probe', r['ok'], r['demux']['rtp_packets'], r['demux']['video_frames'], r['demux']['audio_frames'], r['demux']['errors'][:2])"
  done
  for c in "rtmp-h264-pcmu h264_pcmu.flv gb_udp" "rtmp-h265-aac h265_aac.flv gb_tcp_active"; do
    set -- $c
    out=$R/san-gb-$san/negative-$1
    rm -rf $out
    env LD_LIBRARY_PATH=lib $key=log_path=$R/san-gb-$san/sanitizer-negative-$1$extra python3 regression/gb_ps_negative.py $R/mx-gb-codecs/$1/$3-0.rtp \
      --output $out --demux-bin san-$san/gb_ps_demux --ffmpeg-bin bin/ffmpeg --reference $R/mx-gb-codecs/fixtures/$2 | tail -1
  done
  echo "== $san demux sanitizer reports"
  for f in $(find $R/san-gb-$san -name 'sanitizer-demux*' -o -name 'sanitizer-negative*' | sort); do
    if grep -q "ERROR:\|WARNING: ThreadSanitizer" $f; then echo "FINDING $f"; grep -m3 "ERROR:\|SUMMARY" $f; fi
  done
  echo "reports=$(find $R/san-gb-$san -name 'sanitizer-demux*' -o -name 'sanitizer-negative*' | wc -l)"
done
echo SAN_DEMUX_DONE
