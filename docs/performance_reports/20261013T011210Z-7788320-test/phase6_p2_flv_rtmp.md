# P2：HTTP-FLV 与 RTMP 在相同媒体负载下的 CPU 差异

基线 `7788320af210ab63939c84b6fdcb4e7518674caa`，TEST `<TEST_HOST>`。
本轮不修改生产、测试与 third 代码，不改全局系统设置。

## 1. 结论

（画像归因待 §4 补充。已确定的部分：）

**[实测] HTTP-FLV 比 RTMP 贵 17%（本轮 V800 实测 0.354 mCPU/观看；阶段 5 全区间比值 1.19～1.21）。**
两者 FD 与内存几乎相同，差异**全在用户态**：服务自身封装代码 +0.23、分配器 +0.17（RTMP 的 2.7 倍），
内核反而少 0.11；FLV 的 syscall 次数反而更少（77 vs 113），但每次写多做了 HTTP chunked 封装与分配。

## 2. 权威差值（阶段 5 实测，作为画像归因的锚点）

同一 fixture、同一 RTMP 摄入、1 源，仅改变出流协议。

| viewers | flv | rtmp | FLV/RTMP | 差值（核） |
|---|---|---|---|---|
| 200 | 0.4710 | 0.3925 | 1.20 | +0.079 |
| 400 | 0.9250 | 0.7635 | 1.21 | +0.162 |
| 800 | 1.9604（3 次） | 1.6494（3 次） | 1.19 | +0.311 |
| 1500 | 3.9134（3 次） | 3.2930 | 1.19 | +0.620 |

边际成本（mCPU/观看）：

| 协议 | 各段边际 |
|---|---|
| flv | 2.08 / 2.09 / 2.45 / 2.32 / 2.86 / 2.79 |
| rtmp | 1.86 / 2.21 / 2.35 |

**[实测] 差值 0.22～0.44 mCPU/观看，随负载略增；比值稳定在 1.19～1.21。**

相同点（同档位）：

| 指标 | flv | rtmp |
|---|---|---|
| FD 峰值（V800 / V1500） | 833 / 1533 | 833 / 1533 |
| PSS 峰值（V800 / V1500） | 196.1 / 354.9 MiB | 174.9 / 358.4 MiB |

**[实测] 每观看端 1 个 FD、内存基本相同 → 差异不是连接模型或缓冲差异，而是每字节/每包的 CPU 工作。**

## 3. 方法

与 P1 相同（`pi_phase6p.py` 分相画像 + `pi_phase6g.py` 换算成 mCPU/观看后相减）。
档位选 **V800**：三种协议中唯一在阶段 5 有 3 次重复的档位，便于把画像归因与
阶段 5 的权威值交叉核对。

## 4. 结果（V800，两例均 PASS）

| 用例 | DUT CPU（本轮） | 阶段 5 | 偏差 | mCPU/观看 | PSS 峰值 | FD 峰值 |
|---|---|---|---|---|---|---|
| flv | 1.9162 | 1.9604（3 次） | −2.3% | 2.395 | 200.1 MiB | 833 |
| rtmp（首轮） | 1.6383 | 1.6494 | −0.7% | 2.048 | 175.9 MiB | 833 |
| **rtmp（复跑，干净）** | **1.6327** | 1.6494 | −1.0% | **2.041** | 175.1 MiB | 833 |

媒体健康：两例均 `ready=200 progressing=200 failed=0`（每组）。
**差值：FLV 比 RTMP 多 0.354 mCPU/观看（+17.4%）。**

> 首轮 RTMP 被判定 `TEST_INFRA_FAILURE`，原因是客户端 `measurement_end_monotonic_ns=0`
> （已知的 `bench/media_fanout.h` 测量结束时间戳竞态），媒体本身健康（progressing=200/failed=0）。
> 已复跑一次得到干净结果（上表最后一行），两次 CPU 相差 0.3%，不影响结论。

### 4.1 差异全在用户态，内核反而更省

| DSO | flv | rtmp | 差值（mCPU/观看） |
|---|---|---|---|
| `media_server` | **0.995** | 0.763 | **−0.232** |
| `libjemalloc.so.2` | **0.270** | 0.099 | **−0.171** |
| `[kernel.kallsyms]` | 0.945 | 1.052 | +0.107 |
| `libc.so.6` | 0.136 | 0.096 | −0.041 |
| 画像用户态占比 | **60.4%** | 48.3% | +12.1 pp |

**[实测] FLV 多出的 0.354 mCPU/观看 = 服务自身代码 +0.232 + 分配器 +0.171 + libc +0.041，
内核反而少 0.107。** 其中**分配器工作量是 RTMP 的 2.7 倍**，占差值近一半。

### 4.2 没有单一热点，而是分散在 HTTP 发送路径与分配

self 差值最大项仅 0.055 mCPU/观看，且正负混杂：

| 只在 FLV 侧（boost::beast HTTP 流与缓冲适配） | 值 | 只在 RTMP 侧 | 值 |
|---|---|---|---|
| `boost::beast::basic_stream<…>::write` | 0.018 | `tcp_transport::write` | 0.055 |
| `boost::beast::buffers_compact` 适配 | 0.021 | `tcp_transport::post_write` | 0.044 |
| `asio::write_op<beast…>` | 0.023 | `rtmp_chunk_write` | 0.017 |
| `asio::cancellation_slot::prepare_member` | 0.022 | `asio::executor_function` | 0.015 |
| `asio::detail::initiate_spawn` | 0.016 | `reactive_socket_send` | 0.012 |
| `http_flv_session::run_write` | 0.019 | `__sys_sendto`（内核） | 0.025 |
| `je_eset_remove` / `je_eset_fit`（分配器） | 0.013 / 0.012 | — | — |

**[实测] 差异不是某个函数，而是整条 HTTP chunked 发送路径（beast 的动态缓冲与缓冲区适配器、
cancellation 机制、协程 spawn）加上由此带来的额外分配。**
分配器热点（`je_eset_*`）只在 FLV 侧进入 top 25，与 §4.1 的 2.7× 分配开销一致。

### 4.3 也不是 syscall 次数：FLV 的 syscall 反而更少

| 指标（每观看每秒） | flv | rtmp |
|---|---|---|
| 发送 syscall | **76.78**（`sendmsg`） | **113.46**（`sendto`） |
| `epoll_wait` | 1.30 | 2.17 |
| cycles | 1,946,534 | 1,674,176（**+16%**） |
| instructions | 2,920,009 | 2,536,199（**+15%**） |
| IPC | 1.50 | 1.51（相同） |
| 上下文切换 | 0.48 | 0.50 |
| softirq NET_RX（8 核均值/s） | 6,114 | 6,746 |

**[实测] FLV 用更少、更大的写（chunked 聚合）却多花 15% 的指令，IPC 完全相同。**
即：这是**真实多出来的软件工作量**（封装 + 缓冲 + 分配），不是阻塞、不是缓存效率、也不是 syscall 次数。

## 5. 结论与含义

1. **[实测] HTTP-FLV 比 RTMP 贵 17%（本轮 V800）/ 19%～21%（阶段 5 全区间），即 0.35 mCPU/观看。**
2. **[实测] 差异全在用户态**：服务自身代码 +0.23、分配器 +0.17（RTMP 的 2.7 倍）、libc +0.04，
   内核反而少 0.11。
3. **[实测] 不是 syscall 次数问题**（FLV 77 次 vs RTMP 113 次），而是每次写多做了封装与分配。
4. **观察（不代表本次要改）**：方向是减少 HTTP chunked 发送路径上的临时缓冲与对象分配
   （复用 beast 动态缓冲、避免逐块缓冲区适配），而不是改发送次数。本项未验证。
5. 内存与 FD 基本相同（PSS 200 vs 175 MiB，FD 相同），所以这是纯 CPU 差异。

## 6. 限制

1. 画像窗口 30 s，绝对值以阶段 5 为准，画像只做结构归因。
2. 客户端不同（flv 客户端 vs rtmp 客户端），但归因对象是 DUT 进程 profile，客户端差异不计入。
3. 未改变封装参数（chunk 大小、FLV tag 聚合）验证因果。

## 7. 数据与产物

`phase6fr/summary.jsonl`（flv + rtmp 首轮）、`phase6fr2/summary.jsonl`（rtmp 复跑）、
`phase6fr-all.summary.jsonl`（合并，供对比器使用）、`phase6fr/*.dut.perf.data`、`*.stat.csv`、
`plan6fr.jsonl`、`plan6fr2.jsonl`、`p6chain.sh`；分析输出已归档到本报告目录 `analysis/p2-flv-vs-rtmp-clean.out`。
