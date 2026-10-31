T=/var/tmp/ms-perf-891aec1
NS=msperf-20261013T011210Z-7788320
R=$T/runs4
H=$T/gb_client_harness.py
FAULTS="one_byte eleven_bytes version_1 csrc_beyond_packet extension_beyond_packet padding_flag wrong_payload_type wrong_ssrc sequence_gap continuation_without_pack pack_header_truncated pes_pts_truncated payload_empty oversized pes_length_beyond_packet"
DUMP=$R/mx-gb-codecs/rtmp-h265-aac/gb_tcp_active-0.rtp
cd $T/rt-s
bash $T/env_check.sh $R/san-gb.env.json > /dev/null || { echo "san-gb BLOCKED"; exit; }
for san in asan tsan; do
  if [ $san = asan ]; then key=ASAN_OPTIONS; extra=":detect_leaks=1"; else key=TSAN_OPTIONS; extra=""; fi
  port=20000
  for transport in udp tcp_active tcp_passive; do
    for mode in valid abort $FAULTS; do
      out=$R/san-gb-$san/$transport-$mode
      dump=""; [ $mode = valid ] && dump="--dump-file $out.rtp --viewers 8"
      run_mode=$mode; fault=""
      case $mode in valid|abort) ;; *) run_mode=malformed; fault="--fault $mode";; esac
      ip netns exec $NS python3 $H --gb-fanout san-$san/gb_fanout --dump $DUMP --output $out --transport $transport --mode $run_mode $fault \
        --listen-port $port --env $key=log_path=$out/sanitizer$extra $dump | cut -c1-300
      port=$((port + 20))
      if [ $mode = valid ]; then
        env LD_LIBRARY_PATH=lib $key=log_path=$out/sanitizer-demux$extra python3 bench/gb_av_probe.py $out.rtp --demux-bin san-$san/gb_ps_demux \
          --ffmpeg-bin bin/ffmpeg --expect-audio > $out/probe.json
        python3 -c "import json; r=json.load(open('$out/probe.json')); print('dump-probe', r['ok'], r['demux']['video_frames'], r['demux']['audio_frames'], r['demux']['errors'][:2])"
      fi
    done
  done
  out=$R/san-gb-$san/dump-full
  ip netns exec $NS python3 $H --gb-fanout san-$san/gb_fanout --dump $DUMP --output $out --transport udp --mode valid --viewers 3 \
    --env $key=log_path=$out/sanitizer$extra --dump-file /dev/full | cut -c1-300
  grep -h "phase=dump\|phase=measurement " $out/gb_fanout.log | cut -c1-160
  out=$R/san-gb-$san/dump-unopenable
  ip netns exec $NS python3 $H --gb-fanout san-$san/gb_fanout --dump $DUMP --output $out --transport udp --mode valid --viewers 2 \
    --env $key=log_path=$out/sanitizer$extra --dump-file /nonexistent/dir/x.rtp | cut -c1-300
  tail -2 $out/gb_fanout.log
  for c in "rtmp-h264-pcmu h264_pcmu.flv gb_udp" "rtmp-h265-aac h265_aac.flv gb_tcp_active"; do
    set -- $c
    out=$R/san-gb-$san/negative-$1
    env LD_LIBRARY_PATH=lib $key=log_path=$R/san-gb-$san/sanitizer-negative-$1$extra python3 regression/gb_ps_negative.py $R/mx-gb-codecs/$1/$3-0.rtp \
      --output $out --demux-bin san-$san/gb_ps_demux --ffmpeg-bin bin/ffmpeg --reference $R/mx-gb-codecs/fixtures/$2 | tail -1
  done
  M="--client-dir san-$san --fixtures fixtures --ffmpeg-bin bin/ffmpeg --ffprobe-bin bin/ffprobe --workers 4 --server-cpus 0-3 --client-cpus 8-15"
  ip netns exec $NS bash -c "ulimit -n 1048576; $key=log_path=$R/san-gb-$san/sanitizer-matrix$extra ./run.sh python3 regression/protocol_matrix.py \
    --output $R/san-gb-$san/matrix --server-bin build/media_server $M --only rtmp-h264-pcmu rtmp-h265-aac fault-source-stop gb-udp gb-tcp-active gb-tcp-passive" \
    > $R/san-gb-$san/matrix.log 2>&1
  echo "matrix-$san exit=$?"; cat $R/san-gb-$san/matrix.log
  ip netns exec $NS bash -c "ulimit -n 1048576; $key=log_path=$R/san-gb-$san/sanitizer-multi$extra ./run.sh python3 bench/perf_run.py --output $R/san-gb-$san/multi \
    --server-bin build/media_server --client-dir san-$san --ffmpeg-bin bin/ffmpeg --ffprobe-bin bin/ffprobe --fixture $R/mx-gb-codecs/fixtures/h264_pcmu.flv \
    --host 198.19.0.10 --client-address 198.19.0.11 --workers 4 --server-cpus 0-3 --client-cpus 8-15 --input rtmp --sources 2 \
    --outputs gb_udp:4,gb_tcp_active:4,gb_tcp_passive:4 --gb-peer rtcp_rr --warmup 5 --duration 30 --av-probe 20" > $R/san-gb-$san/multi.out 2>&1
  echo "multi-$san exit=$?"; tail -1 $R/san-gb-$san/multi.out
  python3 -c "
import json; r=json.load(open('$R/san-gb-$san/multi/result.json'))
print(r['status'], [(g['name'], (g.get('measurement') or {}).get('ready'), (g.get('measurement') or {}).get('progressing')) for g in r['groups']])
print({k: (p.get('ok'), p.get('audio_codec'), p.get('av_markers'), (p.get('reference') or {}).get('ok')) for k, p in (r.get('av_probe') or {}).items()})"
  echo "== $san sanitizer reports"
  for f in $(find $R/san-gb-$san -name 'sanitizer*' -type f); do
    if grep -q "ERROR:\|WARNING: ThreadSanitizer\|LeakSanitizer" $f; then echo "FINDING $f"; grep -m3 "ERROR:\|SUMMARY\|WARNING:" $f; fi
  done
  echo "reports=$(find $R/san-gb-$san -name 'sanitizer*' -type f | wc -l)"
done
echo SAN_GB_DONE
