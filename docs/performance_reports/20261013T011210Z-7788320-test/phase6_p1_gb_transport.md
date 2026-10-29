# P1：GB28181 出流的 UDP / TCP(active) / TCP(passive) CPU 归因

基线 `7788320af210ab63939c84b6fdcb4e7518674caa`，TEST `<TEST_HOST>`。
本轮不修改生产、测试与 third 代码，不改全局系统设置。

## 1. 结论

（画像归因待 §4 补充。已确定的部分：）

1. **[实测] active 与 passive 无可测差异**：阶段 5 四个档位差值 −0.2%～+1.0%，
   本轮在 V1000 再做画像级确认：1.8382 vs 1.8386 核（+0.02%），见 §4.5。
2. **[实测] UDP 出流比 TCP 出流贵 30%～36%**：阶段 5 的 5 个档位比值稳定在 1.30～1.40，
   边际成本 2.5 vs 1.9 mCPU/观看（§2）；本轮画像差值 0.661 mCPU/观看。
3. **[实测] 差异全部在内核态（+0.87 mCPU/观看），根因是每个订阅端每个包的 IP 层处理
   与无法合并导致的 NET_RX softirq 放大（5.5×）；syscall 次数两者完全相同（§4.3、§4.4）。**

## 2. 权威差值（阶段 5 实测，作为画像归因的锚点）

同一 fixture、同一 RTMP 摄入、1 源，仅改变出流协议。DUT CPU 为 `perf_run` 测量窗口内的进程 CPU。

| viewers | gb_udp | gb_tcp_active | gb_tcp_passive | UDP/TCP |
|---|---|---|---|---|
| 160 | 0.4429 | 0.3240 | 0.3273 | 1.37 |
| 300 | 0.8065 | 0.5772 | 0.5776 | 1.40 |
| 500 | — | 0.9311 | 0.9295 | — |
| 700 | 1.7720 | 1.3242 | 1.3373（3 次） | 1.34 |
| 1000 | 2.4874（3 次） | 1.9179（3 次） | — | 1.30 |
| 1500 | 3.5616（3 次） | 2.7466 | — | 1.30 |

边际成本（相邻档位差分，mCPU/观看）：

| 协议 | 各段边际 | 典型值 |
|---|---|---|
| gb_udp | 2.66 / 2.60 / 2.22 / 2.52 / 2.38 | **≈2.5** |
| gb_tcp_active | 1.81 / 1.77 / 1.97 / 1.98 / 1.66 | **≈1.9** |
| gb_tcp_passive | 1.79 / 1.76 / 2.04 | **≈1.9** |

**[实测] 差值 = 0.6 mCPU/观看（UDP 比 TCP 贵 30%），且在 160→1500 全区间比值稳定。**
这说明差异是**每观看端每包**的固定成本差，不是某个规模阈值。

结构性差异（同 1000 观看端）：

| 指标 | gb_udp | gb_tcp_active |
|---|---|---|
| FD 峰值 | **2033**（2/观看） | 1033（1/观看） |
| PSS 峰值 | 60.0 MiB | **119.0 MiB** |
| 客户端 CPU（阶段 5 同表） | 2.80 核 | 3.28 核 |

**[实测] UDP 每个订阅端占用 2 个 socket（RTP 与 RTCP 各一），TCP 只有 1 个；
TCP 的内存代价更高（发送缓冲），UDP 的 CPU 代价更高。** 客户端侧则相反（TCP 更贵），
说明两种传输把成本转嫁到了不同位置。

## 3. 方法

- 档位 V1000（三种协议在阶段 5 均健康），1 源，`--group-size 200`，测量窗口 200 s。
- 画像执行器 `pi_phase6p.py` 的分相设计（必须串行，否则 PMU 互相抢占）：
  phase1 DUT 定向 `perf record`（DWARF 调用栈 + 内核样本，30 s）→
  phase2 服务端 8 核系统级 record（20 s）→ phase3 `perf stat`（10 s，独占 PMU，
  含 `sendmsg/sendto/recvmsg/recvfrom/writev/readv/epoll_wait/futex` 的 syscall 计数）。
- 分析器 `pi_phase6g.py`：把两个 profile 的百分比**统一换算成 mCPU/观看再相减**，
  避免只比百分比；DSO 归属用 0.001% 阈值取完整覆盖（阶段 6 §2.2 的教训）。
- 每轮播放的健康判定仍由 `perf_run` 给出（ready/progressing/failed、覆盖率）。

## 4. 结果（V1000，三种传输各一次完整画像，均 PASS）

本轮实测与阶段 5 一致，说明环境差异已排除：

| 用例 | DUT CPU（本轮） | 阶段 5 | 偏差 | mCPU/观看 | PSS 峰值 | FD 峰值 |
|---|---|---|---|---|---|---|
| gb_udp | 2.4989 | 2.4874 | +0.5% | 2.499 | 73.2 MiB | **2033** |
| gb_tcp_active | 1.8382 | 1.9179 | −4.2% | 1.838 | 139.9 MiB | 1033 |
| gb_tcp_passive | 1.8386 | — | — | 1.839 | 134.6 MiB | 1033 |

媒体健康：三例均 `ready=200 progressing=200 failed=0`（每组）。**差值：UDP 比 TCP 多 0.661 mCPU/观看（+36%）。**

### 4.1 差异完全在内核态

| DSO | gb_udp | gb_tcp_active | 差值（mCPU/观看） |
|---|---|---|---|
| `[kernel.kallsyms]` | **1.911** | 1.045 | **−0.866** |
| `media_server` | 0.373 | **0.517** | +0.144 |
| `libc.so.6` | 0.096 | 0.132 | +0.036 |
| `libjemalloc.so.2` | 0.087 | 0.129 | +0.042 |
| 画像内核态占比 | **76.7%** | 56.7% | +20.0 pp |

**[实测] UDP 多出的 0.66 mCPU/观看 = 内核多 0.87 − 用户态少 0.20。**
不是“UDP 把活从用户态搬到内核”这么简单：UDP 总成本更商，而 TCP 反而在用户态多做了事（§4.2）。

### 4.2 机制：每包 IP 层处理 + 无法合并

self 热点差值（mCPU/观看，只列可解释项）：

| 只在 UDP 侧 | 值 | 只在 TCP 侧 | 值 |
|---|---|---|---|
| `udp_sendmsg` / `udp_send_skb` / `__ip_make_skb` | 0.053 / 0.026 / 0.022 | `tcp_sendmsg_locked` | 0.055 |
| `__ip_select_ident`（每包 IP ID） | 0.048 | `tcp_transport::post_write()` | 0.091 |
| `fib_table_lookup`（每包路由） | 0.030 | `tcp_transport::write()` | 0.083 |
| `ip_finish_output2` / `skb_set_owner_w` | 0.036 / 0.023 | `reactive_socket_send` | 0.031 |
| `memset_orig`（内核） | 0.080 vs 0.016 | `std::_Sp_counted_base`（引用计数） | 0.114 vs 0.070 |
| `reactive_socket_sendto` | 0.042 | `boost::asio::…executor_function` | 0.022 |
| `___slab_alloc` / `net_rx_action` / `handle_softirqs` | 0.028 / 0.035 / 0.027 | `_raw_spin_lock_bh` | 0.023 |

**[实测] UDP 对每个订阅端都要完整走一遍 IP 层：建 skb（slab/`memset`）、分配 IP ID、路由查表、
推送；而 TCP 把数据写进已有连接，由协议栈合并成段，不需要每包路由与 IP ID。**
另外 TCP 在用户态多了自己的发送队列与 `shared_ptr` 引用计数（`tcp_transport::post_write` 等），
即 TCP 多出的 0.14 mCPU/观看。

### 4.3 不是 syscall 数量问题，而是每次调用的成本

| 指标（每观看每秒） | gb_udp | gb_tcp_active |
|---|---|---|
| `sendto` 进入次数 | **230.30** | **231.46** |
| `epoll_wait` | 2.26 | 2.65 |
| cycles | 2,273,670 | 1,554,324（−32%） |
| instructions | 2,831,312 | 2,144,356（−24%） |
| IPC | 1.25 | 1.38 |
| 上下文切换 | 0.39 | 0.38 |
| 页缺页 | 2.85 | 3.21 |

**[实测] 两种传输的发送 syscall 次数完全相同（约 231/观看/s，即每包一次），
但 UDP 每次多花 32% 的周期与 24% 的指令。** 这直接排除“UDP 是因为调用次数多”的假设。

### 4.4 softirq：UDP 的接收侧事件多 5.5 倍

| softirq（服务端 8 核均值 /s） | gb_udp | gb_tcp_active |
|---|---|---|
| **NET_RX** | **28,236** | 5,181 |
| SCHED | 99 | 119 |
| TIMER | 4 | 40 |

**[实测] UDP 的 NET_RX 事件是 TCP 的 5.5 倍。** 客户端与服务端在同一主机内，发送包经回环
立即回到接收侧；UDP 每包一个独立数据报无法合并，TCP 的段可被合并/GRO，事件数量少得多。
这与阶段 6 校准中“GB UDP 内核占比 75.8%、NET_RX 主导”一致，并给出了 TCP 对照。

### 4.5 active 与 passive 完全相同

| 指标 | gb_tcp_active | gb_tcp_passive | 差 |
|---|---|---|---|
| DUT CPU | 1.8382 | 1.8386 | **+0.02%** |
| 阻塞事件数（off-CPU） | 28,055 | 28,068 | +0.05% |
| 阻塞总时长占比 | 59.2% | 59.0% | −0.2 pp |

结合阶段 5 四个档位的 −0.2%～+1.0%，**[实测] 连接发起方向对服务端的 CPU 与行为无可测影响。**

### 4.6 无调度竞争

三例的 off-CPU 运行队列等待均 <0.1%（UDP 0.05 s / TCP 0.10 s，共 75 s 采样窗），
阻塞主体是 epoll 等待（52.5% / 59.2% 线程时间）。**[实测] 这些档位下 DUT 不存在 CPU 供给了问题，
差异是纯工作量差异。**

## 5. 结论与含义

1. **[实测] GB UDP 出流比 TCP 贵 30%～36%，差异全部在内核态**，根因是每订阅端每包的 IP 层处理
   （建包/路由/IP ID）与无法合并导致的 softirq 事件放大（5.5×）。
2. **[实测] 不是 syscall 次数差异**（两者均 231 次/观看/s），而是每次调用的内核工作量不同。
3. **[实测] active 与 passive 无差异**，可归为同一传输处理，不必分开建容。
4. **观察（不代表本次要改）**：若要在不改协议语义的前提下降低 UDP 出流成本，方向是减少
   每订阅端的独立发包次数（多订阅端共用发送批次、发送侧 per-destination 缓存），
   而不是减少 syscall 次数。本项未验证，仅由归因方向推出。
5. 资源方向相反：UDP 每订阅端 2 个 socket（RTP+RTCP）但内存更低（73 vs 140 MiB），
   TCP 内存高（发送缓冲）。选型时 CPU / FD / 内存要分开看。

## 6. 限制

1. 画像窗口 30 s，短于阶段 5 的测量窗口；两者口径不同，绝对值以阶段 5 为准，
   画像只用于**结构归因**。
2. `perf record` 自身有开销（阶段 6 已量化：DUT 定向 record 对 CPU 的影响 <1%）。
3. 未改变 GB 侧参数（PS 打包粒度、MTU、RTP 载荷）来验证因果，只做观察归因。

## 7. 数据与产物

`phase6gb/summary.jsonl`、`phase6gb/*.dut.perf.data`、`phase6gb/*.stat.csv`、
`plan6gb.jsonl`、`p6chain.sh`（含每进程 `ulimit -n 65536`）；
分析输出已归档到本报告目录 `analysis/p1-udp-vs-tcp.out`、`analysis/p1-active-vs-passive.out`、
`analysis/p1-offcpu.out`（远端副本在运行目录 `analysis/` 下同名）。
