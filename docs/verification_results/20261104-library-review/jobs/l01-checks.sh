. /var/tmp/ms-perf-891aec1/jobs-lib-l/l.sh
for run in rel asan tsan; do
  name=l1-checks-$run
  ready $name || continue
  mkdir -p $R/$name
  env $(san_env $run $name) bash -c "stdbuf -oL $L/srtp_profile_check-$run 2>&1 | grep -v '\[debug\]'" > $R/$name/srtp.txt
  echo "== $name srtp exit=$? $(tail -1 $R/$name/srtp.txt)"
  env $(san_env $run $name) bash $L/udp_transport_check.sh $L/udp_transport_check-$run $([ $run = rel ] && echo strace) > $R/$name/udp.txt 2>&1
  echo "== $name udp exit=$?"; grep -v '^ok   write accepted' $R/$name/udp.txt
  san_report $name
done
echo L01_DONE
