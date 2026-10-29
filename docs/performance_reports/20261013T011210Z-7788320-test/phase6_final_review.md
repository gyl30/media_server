# 阶段 6 收尾：P0 修正、内存稳定性、GB 传输与 FLV/RTMP 归因

基线 `7788320af210ab63939c84b6fdcb4e7518674caa`，TEST `<TEST_HOST>`，
运行目录 `/var/tmp/media_server_perf/20261013T011210Z-7788320/`。
本轮**未修改生产、测试与 third 代码，未 commit/push，未改全局 sysctl/governor/ulimit/allocator**。

## 1. 执行状态

| 项 | 状态 | 报告 | 说明 |
|---|---|---|---|
| P0 修正（4 项） | **完成** | `phase6_whep_profiling.md` §12 修订记录 | 内存归一化、DSO 覆盖、libSRTP/OpenSSL、实测与推导分离 |
| P0-M 内存稳定性与回收 | **完成（V1200 双周期 + V1500 单周期，播放均 PASS）** | `phase6_p0_memory.md` | A/B/C 三类均已判定 |
| P1 GB UDP/TCP 归因 | **完成（3 例 PASS）** | `phase6_p1_gb_transport.md` | 含 active vs passive 确认 |
| P2 FLV vs RTMP 归因 | **完成（FLV PASS；RTMP 首轮 TEST_INFRA_FAILURE，复跑 PASS）** | `phase6_p2_flv_rtmp.md` | 复跑与首轮 CPU 差 0.3% |
| 优化候选排序 | **完成（只分析，不改代码）** | 本文 §3 | 按可测量地址成本排序 |
| heap profile | **未做** | — | TEST 缺 `jeprof`；改用 `malloc_stats_print` 直接测得 allocated/retained（更贴合问题） |
| off-CPU（`offcputime-bpfcc`） | 工具仍不可用 | 阶段 6 报告 §10 | 用 `bpftrace` 等价实现，本轮 P1 已使用 |

## 2. 本轮新增的事实（按主题）

### 2.1 内存：没有泄漏，但 PSS 会长期高估活跃量

- **[实测] 建立期一次性 0.249～0.262 MiB/观看**（V1200/V1500 两条独立路径，与阶段 6 自管模式的
  0.263/0.287 一致）。
- **[实测] 900 s 稳态零增长**：PSS 斜率 −0.009 / −0.022 MiB/s；周期 12 s 的锯齿（=fixture 循环长度）
  每周期完全回落。
- **[实测] 关闭全部 viewer 后残留不是活跃对象**：jemalloc 自报 allocated **7.5 MiB（V1200）/ 8.4 MiB（V1500）**，
  而内核驻留 315.7 / 249.6 MiB，`LazyFree` 均为 0。
- **[实测] 残留不随轮次叠加**：第二轮同规模 1200 观看端只让水位从 302.8 变到 308.4 MiB（+1.8%），
  且新分配活动触发后 5 s 内归还 ≥222 MiB。
- **机制 [实测]**：`opt.background_thread: false` —— jemalloc 的页衰减只在有分配活动时推进，
  所以空闲时 PSS 平坦、来新连接时立刻归还。两档最终都收敛到约 242 MiB（与观看数无关）。
- **含义**：容量判定必须继续用 `pss_kib_max`；churn 后的 PSS 会高估活跃量约 0.24 MiB × 历史峰值观看端。

### 2.2 GB 出流：UDP 贵 30%～36%，全部在内核态

- **[实测]** UDP 2.499 vs TCP 1.838 核（V1000），差 **0.661 mCPU/观看**；阶段 5 五个档位比值 1.30～1.40。
- **[实测] 根因是每订阅端每包的 IP 层处理**（`udp_sendmsg`/`__ip_make_skb`/`__ip_select_ident`/
  `fib_table_lookup`/skb slab+memset）与**无法合并**：NET_RX softirq 事件 28,236 vs 5,181 /s（5.5×）。
- **[实测] 不是 syscall 次数**：两者发送 syscall 均为 ~231/观看/s；UDP 每次多花 32% 周期、24% 指令。
- **[实测] active 与 passive 无差异**（1.8382 vs 1.8386 核，+0.02%；阶段 5 四档 −0.2%～+1.0%）。
- 资源方向相反：UDP 每订阅端 2 个 socket（RTP+RTCP）但内存更低（73 vs 140 MiB）。

### 2.3 FLV vs RTMP：贵 17%，全部在用户态

- **[实测]** FLV 1.916 vs RTMP 1.633 核（V800），差 **0.354 mCPU/观看**；阶段 5 比值 1.19～1.21。
- **[实测]** 差异构成：服务自身代码 +0.232、**分配器 +0.171（RTMP 的 2.7 倍）**、libc +0.041，
  内核反而 −0.107。
- **[实测]** FLV 的 syscall 更少（77 `sendmsg` vs 113 `sendto`），但多 15% 指令、**IPC 完全相同**
  → 真实多出的软件工作量（HTTP chunked 封装 + beast 动态缓冲 + 分配），不是阻塞或缓存效率。
- 无单一热点（最大项 0.055 mCPU/观看），成本分散在整条 HTTP 发送路径。

### 2.4 对既有阶段结论的影响

- 阶段 6 报告 §7 的内存结论已修正并补强（初稿的"V1800 每观看端 1.3 MiB"是把 `pss_kib_max`
  瞬时峰值差当稳定水位）。
- 阶段 5 的容量表**不受影响**：其 CPU 与 PSS 维度本轮独立复现，偏差 −4.2%～+0.5%。
- 阶段 5 的 GB 结论可以简化：**TCP active 与 passive 可合并为同一传输**建模。

## 3. 优化候选（只分析排序，不代表本次要改）

按"实测可消除成本 × 现实规模"排序。@1500 观看端取阶段 5 的健康上限档位；多源项按 8 源。

| # | 候选 | 实测单位成本 | @1500 观看端 | 占该协议 | 证据 |
|---|---|---|---|---|---|
| 1 | **GB UDP 出流：减少每订阅端的独立发包次数**（发送批次合并、per-destination 缓存） | 0.66 mCPU/观看 | **≈1.0 核** | 26% | P1 §4.1-4.4（DSO+热点+syscall+softirq 四路一致） |
| 2 | **WHEP 视频打包按源共享**（避免每观看端重复解析 NAL/起始码） | 0.33 mCPU/观看 | ≈0.5 核 | 8.8% | 阶段 6 报告 §4.2（`h264_startcode` self 8.8%） |
| 3 | **HTTP-FLV 发送路径：减少临时缓冲与对象分配** | 0.35 mCPU/观看 | ≈0.5 核 | 15% | P2 §4.1-4.3（分配器 2.7×、指令 +15%、IPC 相同） |
| 4 | **SRTP：减少每包的 OpenSSL API 入口**（合并 AAD/数据/标签调用序列） | 0.12 mCPU/观看 | ≈0.18 核 | 3.2% | 阶段 6 报告 §4.6（源码逐行证实每包 5 次入口） |
| 5 | **每源 AAC→Opus 转码**（源维度，非观看维度） | 30 mCPU/源 | 8 源 ≈0.24 核 | — | 阶段 6 报告 §5 |

排序说明：

- 只按**已测到的地址成本**排序，不含实现难度、风险与可维护性。
- 1 与 3 是"减少每订阅端的重复工作/分配"，2 是"按源共享而非按观看端重复"，
  4 是"减少进入库的固定开销次数"——都属于同构方向：**把每观看端每包的成本降下来**。
- **[不成立的方向]** 阶段 6 报告初稿曾提出"在本仓缓存 OpenSSL 算法 fetch"，已由源码证伪
  （本仓与 libSRTP 都没有每包 fetch，`EVP_CIPHER_up_ref/free` 来自 libcrypto 内部）。
- **[不需要做的]** 内存方面没有可修的泄漏（allocated 7.5 MiB）；若希望 PSS 跟随活跃度，
  那是分配器运行选项（`background_thread`）而非代码问题，且容量判定本就用峰值。

## 4. 方法学与工具结论

1. **每进程 FD 上限会直接改变结论**：默认 1024 时约 700 个 WHEP 观看端即耗尽，表现为
   "后几组永远不 ready → all-viewer readiness timed out"。本轮按阶段 5 已批准做法在外层脚本
   `ulimit -n 65536`（仅本测试进程），并在驱动里断言 `/proc/<dut>/limits`。
2. **`perf_run --input existing --server-pid` 是观察"清理后行为"的正确工具**：它明确不停止外部服务，
   因此能在不改测试代码的前提下让 DUT 跨 viewer 关闭存活。阶段边界用 DUT 的 FD 数划分，
   不依赖驱动脚本时钟。
3. **`gdb -p <pid> -batch -ex 'call (void)malloc_stats_print(0,0,0)'` 可在运行时直接读分配器统计**，
   不需要 `jeprof`，也不需要 DUT 正常退出（本 DUT 无信号处理，`stats_print:true` 不会输出）。
4. **`bench/media_fanout.h` 的测量结束时间戳竞态仍在**：本轮 RTMP 首轮 `measurement_end_monotonic_ns=0`
   → coverage 0 → `TEST_INFRA_FAILURE`。这是测试侧缺陷，媒体健康判定不受影响；复跑即可通过。
5. **DSO 归属必须用 0.001% 阈值**：0.01% 会丢掉 2.57%（其中 2.1 pp 属于无符号的 libcrypto）。
6. **等待纪律**：本轮改为远端有界监控循环（30 s 一轮，单行状态含内存/磁盘/PSI/CPU/错误扫描，
   见完成标志即退出），不使用长固定 sleep，也不重复启动仍在运行的测试。

## 5. 限制与未做事项

1. 未做 heap profile（缺 `jeprof`）；但 allocated/retained 已直接测得，性质判定不依赖推断。
2. P0-M 采样间隔 5 s，对 12 s 锯齿存在混叠；真实周期由阶段 6 的 1 s 采样与 fixture 长度确定。
3. 清理后观察 300 s（远大于 `dirty_decay_ms=10000`），但小时级自发归还未观察。
4. P1/P2 的画像窗口 30 s，只用于结构归因；绝对值以阶段 5 与本轮 200 s 窗口为准。
5. 未通过改变参数（GB 打包粒度/MTU、FLV chunk 策略）做因果验证，只做观察归因；
   §3 的方向性建议均标注为未验证。
6. 未测 WHEP 的清理后行为是否受 SRTP/DTLS 资源影响（本轮只覆盖 WHEP 的内存曲线本身）。

## 6. 资源与清理

- 本轮新增原始数据 **780 MB**（预算 8 GiB）；磁盘余量 59 GB；未触发任何安全预算
  （`min-free-memory-mib 8192`、`min-free-disk-mib 2048`、`max-server-pss-mib 16384`）。
- 自有进程（signaling、media_server、perf_publish、采样器、客户端）全部由驱动回收，
  `all_owned_processes_reaped: true`；复核 `pgrep` 无残留。
- 保留：运行包、fixture、基线 worktree、全部 `result.json`/`samples.csv`/`perf.data`/`stat.csv`、
  两份 jemalloc 统计转储。既有 3.6 GB 阶段的 `perf.data` 未删除。
- 测试网络命名空间 `msperf-20261013T011210Z-7788320` 保留（内含 0 个活动进程），
  与阶段 1-5 的做法一致，便于后续复现。

## 7. 产物索引

| 类型 | 路径 |
|---|---|
| 报告 | `phase6_whep_profiling.md`（含 §12 修订记录）、`phase6_p0_memory.md`、`phase6_p1_gb_transport.md`、`phase6_p2_flv_rtmp.md`、本文 |
| 分配器转储 | `jemalloc-after-teardown.txt`（V1200）、`jemalloc-v1500-after-teardown.txt`（V1500） |
| 分析输出（已归档） | 本报告目录 `analysis/`（P1/P2 对比表、off-CPU 汇总、监控日志），远端副本同名 |
| 编排/采样/分析脚本 | `pi_phase6mem2.py`、`pi_phase6samp.py`、`pi_phase6m2a.py`、`pi_phase6g.py`、`pi_phase6fx.py`、`pi_phase6mem.py` |
| 远端数据 | `phase6m-v1200-c2/`、`phase6m-v1500-c1/`、`phase6gb/`、`phase6fr/`、`phase6fr2/`、`phase6fr-all.summary.jsonl` |
| 计划与包装 | `plan6gb.jsonl`、`plan6fr.jsonl`、`plan6fr2.jsonl`、`p6m.sh`、`p6chain.sh`、`p6fr2.sh`、`p6mon.sh` |
