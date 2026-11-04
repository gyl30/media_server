. /var/tmp/ms-perf-891aec1/jobs-lib-u/udp.sh
for pair in gso2:r4 base:r4 base:r5 gso2:r5 gso2:r6 base:r6 base:r7 gso2:r7; do
  v=${pair%:*}; r=${pair#*:}
  mixrun rt-g build/media_server-$v u12-d-2.2-$v-$r D 2.2
done
for d in $R/u9-d-* $R/u12-d-*; do
  [ -d $d ] && echo "$(basename $d) srtp_replay=$(grep -c 'srtp protect failed' $d/server.log 2>/dev/null)"
done
echo U12_DONE
