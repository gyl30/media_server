#!/usr/bin/env bash
# 阶段 6 P0 画像提取：对关键档位导出 self 热点、内核归属、跨档位差分与调用链。
# 只在无负载时运行（perf report 会占用 CPU）。
set -u
T=${T:-/var/tmp/media_server_perf/20261013T011210Z-7788320}
O=$T/phase6prof
mkdir -p $O
R="python3 $T/pi_phase6r.py"

S1_400=$T/phase6w/whep-s1-400-prof1
S1_800=$T/phase6w/whep-s1-800-prof1
S1_1200=$T/phase6wr/whep-s1-1200-prof1r
S1_1500=$T/phase6v/whep-s1-1500-prof1
S8_1200=$T/phase6wr/whep-s8-1200-prof1r

for c in $S1_400 $S1_800 $S1_1200 $S1_1500 $S8_1200; do
  n=$(basename $c)
  $R self $c > $O/$n.self.txt 2>&1
  $R cores $c > $O/$n.cores.txt 2>&1
done

# 跨档位差分：按样本权重比较函数占比变化
perf diff -i $S1_400.dut.perf.data -i $S1_1200.dut.perf.data --stdio > $O/diff_v400_v1200.txt 2>&1
perf diff -i $S1_1200.dut.perf.data -i $S1_1500.dut.perf.data --stdio > $O/diff_v1200_v1500.txt 2>&1
perf diff -i $S1_1200.dut.perf.data -i $S8_1200.dut.perf.data --stdio > $O/diff_s1_s8.txt 2>&1

# 每核内核归属（softirq 与内核函数）
$R cores $S1_1200 > $O/cores_v1200.txt 2>&1
perf report -i $S1_1200.cores.perf.data --stdio --no-children -g none --percent-limit 0.5 \
  --sort symbol > $O/cores_v1200_symbols.txt 2>&1

echo "产物目录: $O"; ls -la $O | head -20
