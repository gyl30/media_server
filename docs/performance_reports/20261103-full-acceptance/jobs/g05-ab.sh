. /var/tmp/ms-perf-891aec1/jobs-lib/mix.sh
for spec in B:1.6 A:1.05 C:2.15; do
  mix=${spec%%:*}; s=${spec#*:}
  mixrun rt-g build/media_server-hist m7-ab-$mix-$s-hist-r1 $mix $s
  mixrun rt-g build/media_server m7-ab-$mix-$s-head-r1 $mix $s
  mixrun rt-g build/media_server m7-ab-$mix-$s-head-r2 $mix $s
  mixrun rt-g build/media_server-hist m7-ab-$mix-$s-hist-r2 $mix $s
done
echo AB_DONE
