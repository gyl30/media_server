#!/bin/bash
# 直接测量不同观看数下服务端 8 核的实际运行频率（Bzy_MHz）。
# 目的：验证高观看档"每观看 CPU 成本下降"是否来自 CPU 调频，而非软件效率变化。
T=/var/tmp/media_server_perf/20261013T011210Z-7788320
NS=msperf-20261013T011210Z-7788320
RT=$T/runtime

for V in 800 1200 1500; do
  OUT=$T/phase6f-$V
  rm -rf $OUT
  TOK=$(openssl rand -hex 32)
  ( cd $RT && nohup bash -c "ulimit -n 65536; ip netns exec $NS env PERF_CONTROL_TOKEN=$TOK ./run.sh python3 bench/perf_run.py \
      --output $OUT --input rtmp --outputs whep:$V --sources 1 --warmup 20 --duration 70 \
      --group-size 200 --host 198.19.0.10 --client-address 198.19.0.11 \
      --rtmp-port 11935 --rtsp-port 18554 --http-port 18080 --workers 8 \
      --server-cpus 0-7 --client-cpus 8-15 \
      --min-free-memory-mib 8192 --min-free-disk-mib 2048 --max-server-pss-mib 16384" >/dev/null 2>&1 & )
  sleep 42
  echo "=== whep:$V  服务端 8 核实测频率 ==="
  PID=$(pgrep -x media_server | head -1)
  timeout 30 turbostat --interval 5 --num_iterations 2 --quiet --cpu 0-7 2>/dev/null |
    awk 'NR==1{for(i=1;i<=NF;i++)h[$i]=i;next}{if(h["Bzy_MHz"]){printf "%s ", $h["Bzy_MHz"]; if(h["Busy%"])printf "(busy %s) ", $h["Busy%"]}}END{print ""}'
  echo "  DUT 瞬时核数: $(awk '{print ($14+$15)/100}' /proc/$PID/stat 2>/dev/null) (累计 utime+stime 秒)"
  pkill -f "perf_run.py.*phase6f-$V"; sleep 6
done
pkill -x media_server 2>/dev/null; sleep 3
rm -rf $T/phase6f-800 $T/phase6f-1200 $T/phase6f-1500
echo DONE
