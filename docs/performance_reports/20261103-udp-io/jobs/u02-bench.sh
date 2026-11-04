. /var/tmp/ms-perf-891aec1/jobs-lib-u/udp.sh
OUT=$R/u2-bench.jsonl
: > $OUT
bench() { ip netns exec $NS bash -c "ulimit -n 65536; exec $U/udp_batch_bench --sender-cpu 2 --receiver-cpu 4 --receivers 2 --seconds 5 $*" >> $OUT 2>&1; }
for rep in 1 2 3; do
  for size in 200:200 600:600 1216:700; do
    s=${size%:*}; last=${size#*:}
    for burst in 1 4 8 16; do
      l=$last; [ $burst = 1 ] && l=$s
      for layout in "--sockets 1000 --rounds-per-second 30" "--sockets 8 --rounds-per-second 2000"; do
        modes="sendto connected sendmmsg gso"
        [ $burst = 1 ] && modes="sendto connected sendmmsg"
        [ $rep = 2 ] && modes=$(echo $modes | tr " " "\n" | tac | tr "\n" " ")
        for mode in $modes; do bench --mode $mode --size $s --last-size $l --burst $burst $layout; done
      done
      bench --mode sendmmsg --size $s --last-size $l --burst $burst --sockets 4 --endpoints-per-socket 64 --rounds-per-second 100
      bench --mode sendto --size $s --last-size $l --burst $burst --sockets 4 --endpoints-per-socket 64 --rounds-per-second 100
    done
  done
  for mode in sendmmsg gso; do bench --mode $mode --size 1216 --last-size 700 --burst 8 --sockets 1000 --rounds-per-second 30 --copy 1; done
done
python3 $U/bench_sum.py $OUT
echo U02_DONE
