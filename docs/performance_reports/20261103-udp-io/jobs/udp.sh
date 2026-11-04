. /var/tmp/ms-perf-891aec1/jobs-lib/mix.sh
R=$T/runs8
mkdir -p $R
bash $T/cleanup.sh runs8 > /dev/null
U=$T/jobs-lib-u
COMMON_U=$(echo "$COMMON" | sed 's/ --source-churn 20 --churn rtmp:100 --av-probe 20//')
urun() {
  rt=$1; bin=$2; name=$3; in=$4; out=$5; shift 5
  ready $name || return 0
  cd $T/$rt
  ip netns exec $NS bash -c "ulimit -n 1048576; ./run.sh python3 bench/perf_run.py --output $R/$name \
    --server-bin $bin --client-dir build --ffmpeg-bin bin/ffmpeg --ffprobe-bin bin/ffprobe \
    --fixture .cache/perf_fixture_720p30.flv --whip-fixture .cache/perf_fixture_whip.mkv \
    --host 198.19.0.10 --client-address 198.19.0.11 --workers 8 --server-cpus 0-7 --client-cpus 8-15,24-31 \
    --min-free-memory-mib 8192 --min-free-disk-mib 4096 --max-server-pss-mib 16384 --inputs $in --outputs $out $COMMON_U $*" > $R/$name.out 2>&1
  echo "== $name rt=$rt bin=$bin inputs=$in outputs=$out exit=$?"
  python3 $T/sum.py $R/$name
  python3 $T/jobs-lib/life_summary.py $R/$name | cut -c1-1200
  find $R/$name -type f \( -name "*.dump" -o -name "*.mkv" -o -name "*.rtp" -o -name "*.ts" -o -name "*.flv" -o -name "*.es" \) -size +100k -delete
  gzip -f $R/$name/result.json
}
uprof() {
  name=$1; shift
  P=$R/prof-$name
  rm -rf $P; mkdir -p $P
  "$@" > $P/run.txt 2>&1 &
  runner=$!
  until [ -e $R/$name/measurement.start ] || ! kill -0 $runner 2>/dev/null; do sleep 1; done
  sleep 20
  server=$(pgrep -f "threads 8 --bind-address 198.19.0.10" | head -1)
  echo "server=$server" > $P/pids.txt
  cat /proc/softirqs > $P/softirqs-before.txt
  taskset -c 15 perf stat -e cycles,instructions,context-switches,page-faults -C 0-7 -- sleep 10 > $P/stat.txt 2>&1
  cat /proc/softirqs > $P/softirqs-after.txt
  taskset -c 15 perf record -a -C 0-7 -g -F 499 -o $P/server.data -- sleep 15 > $P/record.log 2>&1
  taskset -c 15 sh $U/udp_trace.sh $server > $P/trace.txt 2>&1
  wait $runner
  cat $P/run.txt
  perf script -i $P/server.data 2>/dev/null | gzip > $P/perf-script.txt.gz
  perf report --stdio --no-children -g none --sort dso,sym -i $P/server.data 2>/dev/null | grep -E "^ +[0-9]" | head -300 > $P/flat.txt
  rm -f $P/server.data $P/server.data.old
  python3 $U/udp_attr.py $P/perf-script.txt.gz | tee $P/attr.txt
  python3 $U/trace_sum.py $P/trace.txt $P/stat.txt | tee $P/trace-sum.txt
}
utrace() {
  name=$1; shift
  P=$R/trace-$name
  rm -rf $P; mkdir -p $P
  "$@" > $P/run.txt 2>&1 &
  runner=$!
  until [ -e $R/$name/measurement.start ] || ! kill -0 $runner 2>/dev/null; do sleep 1; done
  sleep 20
  server=$(pgrep -f "threads 8 --bind-address 198.19.0.10" | head -1)
  taskset -c 15 perf stat -e cycles,instructions -C 0-7 -- sleep 10 > $P/stat.txt 2>&1
  taskset -c 15 sh $U/udp_trace.sh $server > $P/trace.txt 2>&1
  wait $runner
  cat $P/run.txt
  python3 $U/trace_sum.py $P/trace.txt $P/stat.txt | tee $P/trace-sum.txt
}
