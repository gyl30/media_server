. /var/tmp/ms-perf-891aec1/jobs-lib-r/r.sh
for r in 1 2 3 4 5 6 7; do
  for v in fix old; do
    B=$([ $v = fix ] && echo build/media_server || echo $T/rt-l/build/media_server)
    mixrun rt-r $B r2-d-2.2-$v-r$r D 2.2
    echo "== r2-d-2.2-$v-r$r replay_fail=$(grep -c 'srtp protect failed' $R/r2-d-2.2-$v-r$r/server.log 2>/dev/null) rtmp_play=$(grep -c 'rtmp play' $R/r2-d-2.2-$v-r$r/server.log 2>/dev/null) whep_started=$(grep -c 'whep session started' $R/r2-d-2.2-$v-r$r/server.log 2>/dev/null)"
  done
done
echo R02B_DONE
