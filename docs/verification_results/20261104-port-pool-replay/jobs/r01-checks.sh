. /var/tmp/ms-perf-891aec1/jobs-lib-r/r.sh
for run in rel asan tsan; do
  name=r1-checks-$run
  ready $name || continue
  mkdir -p $R/$name
  env $(san_env $run $name) $L/port_pool_check-$run > $R/$name/port_pool.txt 2>&1
  echo "== $name port_pool exit=$? $(tail -1 $R/$name/port_pool.txt)"
  env $(san_env $run $name) $L/rtp_ssrc_check-$run $([ $run = rel ] && echo 200000 || echo 20000) 20 > $R/$name/rtp_ssrc.txt 2>&1
  echo "== $name rtp_ssrc exit=$?"; cat $R/$name/rtp_ssrc.txt
  env $(san_env $run $name) $L/rtp_ssrc_check-old-$run $([ $run = rel ] && echo 200000 || echo 20000) 20 > $R/$name/rtp_ssrc_old.txt 2>&1
  echo "== $name rtp_ssrc_old exit=$?"; cat $R/$name/rtp_ssrc_old.txt
  san_report $name
done
echo R01_DONE
