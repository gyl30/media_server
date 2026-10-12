# `perf_profile.py`：独立性能诊断

本工具只采集已运行 DUT 或解析已有 heap dump，不启动媒体服务、不修改 sysctl、不替换 allocator、不停止 DUT。诊断结果不能与未开启 profiler 的容量结果混算。`PASS` 表示取得了本次模式要求的有效证据，不表示媒体健康或没有性能瓶颈。

## 依赖与参数

运行包入口：`./run.sh python3 bench/perf_profile.py --help`。Python 3.10+、Linux `/proc`；真实采样在 TEST 使用该环境实际可用且有权限的工具。工具不会安装依赖或调整权限。

| 参数 | 约束及默认值 |
|---|---|
| `--mode` | 必填：`cpu-stat`、`cpu-record`、`offcpu`、`heap-report` |
| `--output` | 必填、新目录，禁止覆盖；创建权限 0700 |
| `--pid` | 正整数，前三种模式必填，heap-report 禁止 |
| `--expected-starttick` | 可选 Linux `/proc/PID/stat` 第 22 字段，用于拒绝错误/复用 PID |
| `--duration` | 正整数秒，默认 30；live 模式采样请求窗口，不是精确 PMU armed 时间 |
| `--perf-bin` | 默认 `perf`，用于 CPU 模式 |
| `--offcpu-bin` | 默认 `offcputime-bpfcc`，输出 folded stacks |
| `--binary`、`--heap-file` | heap-report 两项必填，live 模式禁止；必须是准确匹配的 ELF 和单个非空 heap dump |
| `--jeprof-bin` | 默认 `jeprof` |

## 可直接执行的最小示例

以下命令在独立 TEST 运行包目录执行；`DUT_PID` 必须来自本轮拥有的运行记录，而非通过泛化 `pgrep` 猜测。三种 live 诊断应各自使用独立、相同负载的运行，不与容量基线或其他重型 profiler 叠加。

```bash
export DUT_PID=12345 # 替换成已核实的本轮服务 PID
export DUT_STARTTICK=$(python3 -c 'import os,pathlib; print(pathlib.Path("/proc/"+os.environ["DUT_PID"]+"/stat").read_text().rsplit(") ",1)[1].split()[19])')
./run.sh python3 bench/perf_profile.py --mode cpu-stat \
  --pid "$DUT_PID" --expected-starttick "$DUT_STARTTICK" --duration 30 \
  --output results/diagnostic-cpu-stat
./run.sh python3 bench/perf_profile.py --mode cpu-record \
  --pid "$DUT_PID" --expected-starttick "$DUT_STARTTICK" --duration 30 \
  --output results/diagnostic-cpu-record
./run.sh python3 bench/perf_profile.py --mode offcpu \
  --pid "$DUT_PID" --expected-starttick "$DUT_STARTTICK" --duration 30 \
  --output results/diagnostic-offcpu
```

不能重新使用已有输出路径。此处仅规定操作步骤；不能把这些示例当作 TEST 已采样证明。

heap-report 不负责产生 dump。先确认运行进程真实使用支持 profiling 的 jemalloc，并在进程启动前配置独立的 `MALLOC_CONF`/dump 路径；仅设置环境变量不证明 allocator 生效。不对他人的运行服务附加新的 preload。

```bash
export HEAP_FILE=/var/tmp/my-diagnostic/server.12345.0.f.heap # 替换成一个真实文件，不能用多文件 glob
./run.sh python3 bench/perf_profile.py --mode heap-report \
  --binary build/media_server --heap-file "$HEAP_FILE" \
  --output results/diagnostic-heap
```

## 输出与验收

- `result.json`：status/reason、host、driver SHA、请求 duration、步骤命令、profiler PID、tool SHA、单调时钟开始/结束、elapsed 和退出码。live 模式记录 DUT PID/starttick/ELF SHA；heap 模式记录 ELF/heap SHA。
- cpu-stat：`perf-stat.txt`，固定六项 `task-clock,context-switches,cpu-migrations,page-faults,cycles,instructions`，TAB 分隔。JSON 保留 `counter_units`，task-clock 通常为毫秒、其余为事件计数；这些不是 CPU 百分比。全部计数须有效，task-clock 必须正；缺失/不支持不记为 0。
- cpu-record：`perf.data`、`perf-record.log`、`perf-report.txt` 及 raw SHA。当前 `perf record -F 99 -g`；必须存在正样本数。默认 call graph 不保证所有栈均完整，必须另查 unknown/lost/符号质量，不把 inclusive 比例相加。
- offcpu：`offcpu-folded.txt`，至少一条正等待量的 folded stack；等待量单位微秒，多个线程时间可相加，不能当作单线程 wall latency。警告或零字节输出不证明零竞争，epoll 正常等待不是直接瓶颈。
- heap-report：`jemalloc.txt`，要求 jeprof 提供字节总量；可为合法 0 活跃分配，不等于依赖栈全无泄漏。采样分配/allocator retained/RSS/PSS 必须分开解释。

工具整体步骤 elapsed 包括初始化、符号解析和停止，不等于实际纯采样时长；真实 PMU/BPF 采样边界和 lost event 要从原始结果另外核对。PID 在采样前后必须对应同一 starttick；目标退出或复用为 BLOCKED。

`PASS` 返回 0；依赖缺失、权限拒绝、无有效计数/样本/栈、目标失效或子命令失败返回 1、状态 BLOCKED，并保留 raw/log。参数错误由 argparse 返回 2。不要将 BLOCKED 换算为零 CPU、零 Off-CPU 或零分配。

## 资源、清理与验证边界

所有诊断子进程使用 DEVNULL stdin 和本工具创建的独立进程组；超时或完成后清理该组的遗留 helper，不触及 DUT 或其他 Agent。cpu-stat/record 子命令上限 duration+60 秒，offcpu duration+90 秒，离线 report 120 秒，关闭再有最多约 4 秒等待。

离线回归 `tests/perf_profile_acceptance.py` 覆盖空统计、缺计数、零样本、warning 假栈、DEVNULL、归属字段和超时 helper 清理；fake 输出不是 TEST 性能证据。实际高负载 perf/BCC/heap 由 Pi 在 TEST 首次验收，缺权限记 BLOCKED，不绕过保护。

原始栈、路径、ELF、dump 可能包含敏感资产；只保存到受限独立目录，未经检查脱敏不得公开。诊断源、binary、fixture SHA 应与本轮 workload 原始结果关联。
