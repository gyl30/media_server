. /var/tmp/ms-perf-891aec1/jobs-lib/mix.sh
profrun() {
  rt=$1; bin=$2; name=$3; mix=$4; s=$5; shift 5
  P=$R/prof-$name
  rm -rf $P; mkdir -p $P
  mixrun $rt $bin $name $mix $s "$@" > $P/run.txt 2>&1 &
  until [ -e $R/$name/measurement.start ] || ! kill -0 $! 2>/dev/null; do sleep 1; done
  sleep 25
  taskset -c 15 perf record -F 499 -g -C 0-7 -o $P/server.data -- sleep 15 > $P/record.log 2>&1
  taskset -c 15 perf stat -e cycles,instructions,context-switches,cpu-migrations,page-faults -C 0-7 -- sleep 10 > $P/stat.txt 2>&1
  wait
  cat $P/run.txt
  perf report --stdio --no-children -g none --sort dso,sym -i $P/server.data 2>/dev/null | grep -E "^ +[0-9]" > $P/flat.txt
  perf report --stdio --no-children -g none --sort tid -i $P/server.data 2>/dev/null | grep -E "^ +[0-9]" | head -20 > $P/tid.txt
  python3 $T/jobs-lib/classify_profile.py $P/flat.txt
  head -40 $P/flat.txt
  cat $P/tid.txt $P/stat.txt
  rm -f $P/server.data $P/server.data.old
}
