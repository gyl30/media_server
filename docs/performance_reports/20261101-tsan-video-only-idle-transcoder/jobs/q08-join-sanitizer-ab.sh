T=/var/tmp/ms-perf-891aec1
NS=msperf-20261013T011210Z-7788320
R=$T/runs5
udp() { ip netns exec $NS awk '/^Udp:/ {n++; if (n == 2) print "InErrors=" $4, "RcvbufErrors=" $6}' /proc/net/snmp; }
for rep in 1 2 3; do
  for san in asan tsan; do
    for variant in a b; do
      name=q-joinab-$san-$variant-r$rep
      rm -rf $R/$name
      bash $T/env_check.sh $R/$name.env.json > /dev/null || { echo "$name BLOCKED"; continue; }
      before=$(udp)
      ip netns exec $NS python3 $T/aac_join.py --output $R/$name --runtime $T/rt-s --server-bin build-t/media_server-$variant-$san --input whip --expect-restarts 1 --lead 12 > $R/$name.out 2>&1
      after=$(udp)
      echo "== $name exit=$? udp_before[$before] udp_after[$after] $(tail -1 $R/$name.out)"
      python3 -c "
import json; w=json.load(open('$R/$name/result.json'))['whep_viewer']; print({k: w.get(k) for k in ('audio_frames', 'audio_pts_gaps', 'audio_pts_gap_max_ms', 'video_pts_gaps', 'av_markers')})"
      echo "sanitizer lines: $(grep 'Sanitizer\|runtime error' $R/$name/server.log | grep -vc 'makecontext/swapcontext')"
    done
  done
done
echo JOINAB_DONE
