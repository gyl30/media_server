# UDP 网络路径归因、批量 I/O 优化与固定 8 核验收（2026-11-04，基线 `b131705`）

结论：**GO（UDP GSO 帧内合并，ACCEPT）**。UDP 发送是 GB UDP 与 WHEP 扇出的首要成本；隔离实验中 GSO 每包 cycles −45%～−69%，`sendmmsg` 只有 −4%～−12%（burst 1 时回退），连接 socket −5%～−11%。生产改动只在一帧的 RTP 包同步产出期间把同目的、等长的数据报合并成一次 `UDP_SEGMENT` 发送，不加定时器、不跨帧、不改 SRTP 粒度。固定 8 核 A/B：GB UDP 4000 路 Gcyc/s −46%，WHEP 1536 路 −12%，混合 D −5.6%、A −3.9%、B −2.4%、C −0.9%；健康容量 D 2.2×→2.4×、B 1.6×→1.7×。协议矩阵、Sanitizer、浏览器、生命周期与 GB 音画检查通过。遗留一个与本改动无关、此前已出现过的 WHEP SRTP `replay_fail`（第 9 节）。

标记口径：PASS / FAIL / XFAIL / BLOCKED / INCONCLUSIVE。未注明的结果均为本轮实测；“推断”单独注明。生产代码未提交，等待统一审查。

## 1. Git 与构建基线

| 项 | 值 |
| --- | --- |
| 基线 | `origin/main` = HEAD = `b131705`；生产源码自 `0eae004` 未变 |
| `media_server-base`（HEAD 源码） | sha256 `edca2c36b236a5cc…` |
| `media_server-gso`（第一版候选，每帧分配暂存区，已废弃，测试机上已删除） | `0f5553f3f8763540…` |
| `media_server-gso2`（最终候选 = 工作区 diff） | `275b70eb667e2715…` |
| ASan+UBSan / TSan（候选） | `6f45589471883a46…` / `20703f5692192bca…`；HEAD TSan `2812ff91102523fe…` |
| 客户端、上游 | `whep_fanout` `1836a5d5…`、`gb_fanout` `8e2cf96c…`、上游 media_server（`rt-e1`）`9d37de83…`，与上轮相同 |
| 素材 | 负载 `perf_fixture_720p30` `59c81471…`、`perf_fixture_whip` `8581fcd9…`；音画验证另用矩阵素材 `avsync_720p30` `750690a7…`、`avsync_whip` `853fd8d8…` |
| 构建 | RelWithDebInfo，GCC 16，`deps/install`（OpenSSL 3.5.9、Boost 1.92、jemalloc 5.3.0），静态运行时；base 与 gso2 在同一工作树先后构建（`git stash` 切换），仅源码差异 |
| 环境 | `<TEST_HOST>`，内核 6.8.0-84，schedutil；DUT 绑 CPU 0-7、8 worker；客户端 8-15,24-31；netns 内 DUT 198.19.0.10 / client 198.19.0.11（同一 netns，走 lo）；sysctl、qdisc、网卡参数均未改 |

## 2. UDP 收发 CPU 与系统调用归因

工具：`udp_trace.sh`（服务端每个系统调用的次数与耗时、`udp_sendmsg`/`udp_recvmsg` 长度分布、同 fd 连续发送串长）、`udp_attr.py`（`perf record -a -C 0-7 -g` 调用栈分类）、`trace_sum.py`。运行：`jobs/u01-attr.sh`、`jobs/u03-retrace.sh`。perf 与 bpftrace 开销使混合负载在归因运行中出现丢包，这些运行只用于归因，不计容量。

### 2.1 服务端 CPU 构成（占采样 cycles）

| 负载 | UDP 发送 | 其中 lo 交付 | 用户态主要项 | 其他 |
| --- | --- | --- | --- | --- |
| GB UDP 4000（rtmp:16） | 83.5% | 24.9% | 打包 4.3%、transport 2.3% | 系统调用进出 5.7% |
| WHEP 1536（whip:32） | 55.5% | 16.4% | SRTP 14.0%、打包 7.9% | — |
| WHIP 160→WHEP 160 | 28.3% | 9.1% | SRTP（解密为主）15.8%、打包 13.6% | UDP 接收 8.8%、epoll 5.9% |
| D 2.2× | 33.0% | 10.6% | 转码 22.0%、SRTP 10.3% | TCP 1.7% |
| B 1.6× | 20.9% | 6.5% | 转码 28.2%、SRTP 6.8% | TCP 10.8% |
| A 1.0× | 19.3% | 5.1% | 转码 9.8% | TCP 31.7% |
| C 2.15× | 5.9% | 1.5% | 转码 38.6% | TCP 10.2%、UDP 接收 5.0% |

“lo 交付”是在发送系统调用里内联执行的接收端 softirq（loopback 特有；真实网卡上这部分在客户端主机）。GB 发送路径细分：协议栈其他 24.8%、skb 分配/释放 10.1%、拷贝/校验和 8.7%（其中 `memset` 6.1%，内核 `init_on_alloc` 清零 skb 数据，属内核配置）、路由 3.8%、锁 3.5%、IP id 3.2%、syscall 入口 2.5%、AppArmor 2.0%。接收、加密、转码不属于发送路径，不计入发送优化上限。

### 2.2 系统调用与包

| 负载 | UDP 发送 k/s | 平均 µs/次 | 折合核 | 平均包长 | 同 fd 连续串长 | 处在 ≥4 串中的调用 |
| --- | --- | --- | --- | --- | --- | --- |
| GB 4000 | `sendto` 1103 | 4.91 | 5.42 | ~1030 B（72% 为 1200 B 段） | 3.13 | 72.7% |
| WHEP 1536 | `sendto` 692 | 5.35 | 3.70 | ~623 B | 5.13 | 87.6% |
| D 2.2× | `sendto` 756 | 3.99 | 3.01 | ~655 B | 8.44 | 95.3% |
| B 1.6× | `sendto` 438 | 4.28 | 1.88 | ~624 B | 2.86 | 69.6% |
| A 1.0× | `sendto` 315 | 4.08 | 1.29 | ~796 B | 1.55 | 37.9% |
| C 2.15× | `sendto` 200 | 4.52 | 0.90 | ~771 B | 1.66 | 40.7% |

A/C 的 `sendmsg`（334k、203k/s）主要是 TCP。串长说明：一帧的 RTP 包由打包器在一次调用里同步产出（WHEP `rtsp_muxer_input`、GB `rtp_payload_encode_input`），同一会话的包连续写同一 socket，这是批量化的天然边界；每个会话固定一个对端，WHEP 不能 `connect`（ICE/STUN 可能来自其他地址）。

## 3. `sendmmsg`、GSO 隔离实验

`udp_batch_bench.cpp`（`jobs/u02-bench.sh`，以 u04 运行）：发送线程绑一核，`perf_event_open` 计 cycles、user cycles、instructions；接收端 `recvmmsg` 校验每个数据报的 endpoint、长度、序号与填充内容。模式：`sendto`、`connected`（`connect` 后 `send`）、`sendmmsg`（处理部分成功并续发）、`gso`（多 iovec + `UDP_SEGMENT`）。尺寸 200、600、1216（末包 700）；每轮 1/4/8/16 包；布局 8 socket×2000 轮/s、1000 socket×30 轮/s、4 socket×64 endpoint；每组 3 次，第 2 次反转模式顺序。全部配置完整性 ok，EAGAIN 与短发送 0。

1216/700（接近 MTU，GB/WHEP 视频主体）的中位 cycles/包：

| 每轮包数 | 布局 | sendto | connected | sendmmsg | gso |
| --- | --- | --- | --- | --- | --- |
| 1（1216） | 8×1 | 9473 | −9.1% | +6.4% | — |
| 4 | 8×1 | 7959 | −8.7% | −6.6% | −46.2% |
| 4 | 1000×1 | 8634 | −11.3% | −8.9% | −54.0% |
| 8 | 8×1 | 7670 | −8.7% | −9.0% | −59.6% |
| 8 | 1000×1 | 7892 | −7.3% | −6.5% | −59.2% |
| 16 | 8×1 | 7855 | −9.0% | −9.3% | −68.6% |
| 4 | 4×64 endpoint | 7735 | — | −3.8% | 不适用 |

- 200 B、600 B 结论相同：GSO −45%（4 包）～−68%（16 包）；`sendmmsg` 每轮 1 包时 +5%～+9%，≥4 包时 −4%～−12%。
- `sendmmsg` 只摊薄系统调用进出；内核仍对每条消息执行 `___sys_sendmsg`、安全钩子、路由与 skb 分配。GSO 一次调用只建一个 skb、做一次路由和 IP id，在 lo 上于接收端 `udp_rcv_segment` 分段。
- 需要复制到连续缓冲（生产实现的形态）时 GSO 3364 对 3219 cycles/包（+4.5%），收益基本保留。
- 延迟：8×1 布局 GSO p50 只多 10–25 µs（一次交付整批）；1000×1 布局下 p50/p99 高是测试接收端单线程排队所致，`sendto` 同布局 burst 1 也有 550–700 µs，不代表 GSO 引入的发送延迟。
- 多 endpoint：`sendmmsg` 可跨 endpoint 但收益 −4%～−12%；GSO 要求同一目的，生产中每个会话只有一个对端，此限制不影响适用面。

## 4. 各候选 ACCEPT / REJECT

| 候选 | 判定 | 依据 |
| --- | --- | --- |
| UDP GSO（同目的、等长段，帧内合并） | **ACCEPT** | 隔离 −45%～−69%；8 核 GB −46%、WHEP −12%，D/B 健康容量提高；功能与生命周期检查通过 |
| `sendmmsg` | REJECT | 收益 ≤12%，同样需要批量化改造，burst 1 回退 +4%～+9%，被 GSO 完全覆盖 |
| `connect` 后发送 | REJECT | −5%～−11%，只适用于 GB（WHEP 不能 connect）；GSO 后路由与 IP id 已降为每批一次，剩余收益更小，不单独叠加 |
| 基于 post 的批量 flush | REJECT（设计阶段） | `worker_sink_dispatcher::publish` 在一个 handler 内把帧发给所有 sink，post 的 flush 会跨会话累积、推迟首包，等同人为积累延迟 |
| 定时器成批 | REJECT（设计阶段） | 增加延迟，帧内同步合并已能拿到整帧批次 |
| 第一版 GSO：每帧 `reserve` 63 KiB 暂存区 | REJECT（实测） | WHEP 1536 Gcyc/s +4%、B +3%：jemalloc 大块 extent 分配/释放（`je_arena_extent_alloc_large` 等）抵消收益；改为每线程复用缓冲后 WHEP −12% |

不修改 sysctl、内核队列或网卡参数；`init_on_alloc` 清零、AppArmor 钩子、lo 交付属环境，不计入可优化项。

## 5. 生产代码改动

`git diff --stat`：6 个文件，+193/−18。

**`media/net/udp_transport.{h,cpp}`**

- 新增 `cork()` / `uncork()`。corked 且写队列为空时，`write()` 把数据报追加到每线程复用的暂存缓冲（`thread_local held_buffer`，记录持有者 transport），记录段长与目的地址，返回 true。
- 立即发送暂存批次的条件：目的地址不同、长度大于首段、累计超过 63 KiB 或 64 段（内核 `UDP_MAX_SEGMENTS` 128 内）；短于首段的包作为最后一段并立即发送；`uncork()` 发送剩余。另一个 transport 开始暂存前，先把当前持有者的批次发出，同一 worker 上的多个 transport 不会互相覆盖。
- 发送：1 段走原 `send_to`；多段用 `sendmsg` + `SOL_UDP/UDP_SEGMENT` cmsg，`MSG_DONTWAIT|MSG_NOSIGNAL`。
- 失败处理：`EAGAIN`/`EWOULDBLOCK` 时各段按原顺序进入原写队列（受 1 MiB 高水位约束，超出部分被丢弃，与原单包路径相同）；`EINVAL`/`EIO`（段长或校验和卸载不支持 GSO）时逐段走原单包路径；其他错误通过 `error_callback` 上报一次（post 到 executor，与原实现相同）。
- 写队列非空时不暂存，直接走原排队路径，保证批次不越过已排队的包；`shutdown()` 清除暂存。
- 原 `write` 中的单包发送、入队、错误上报分别提取为 `send_now`、`enqueue`、`report_error`，行为不变。

**`media/webrtc/whep_session.cpp`**：`on_frame` 在 `packetizer_->on_frame(frame)` 前后 `cork()`/`uncork()`。每个 RTP 包仍各自 SRTP 加密后再写入，GSO 段就是独立的 SRTP 包。

**`media/gb28181/gb28181_rtp_sender.{h,cpp}`、`gb28181_udp_sender_session.cpp`**：sender 新增可选的帧完成回调，在 `rtp_payload_encode_input` 返回后调用；UDP sender 在 `send_packet` 中 `cork()`，帧完成回调里 `uncork()`；`safe_shutdown` 清空回调。TCP sender 不使用。

语义检查（对照 Goal 第 3 阶段 9 项）：数据报边界、顺序和目的地址不变（每段对应一个原数据报，`udp_transport_check` 与 u13 验证）；1 MiB 高水位与溢出语义不变；在途 buffer 只在原写队列中（暂存数据要么同步发出，要么复制进队列）；所有操作在同一 worker 上串行；写错误仍经 callback；关闭仍由 session 决定；SRTP/RTP 序号、RTCP 计数不变（加密与计数在 write 之前）；不新增定时器、队列或生命周期状态。

### 5.1 transport 定向检查（`udp_transport_check.cpp`、`udp_transport_check.sh`，结果 `runs8/u5-transport-check`）

直接链接 `media/net/udp_transport.cpp`；Release、ASan、TSan 三个版本全部 PASS（796 个断言）。

- 分组：5×1216+700；200、1216×3、500、1216、另一目的 300、1216、1300；70×1216（64+6 两批）；两个 transport 在同一线程交错暂存；0 字节数据报。两个接收端收到的序列与发送完全一致。strace：5 个 GSO 批次成功（6780、4148、64448、20672、2432 字节），1 个 `EINVAL` 后逐段回退，`sendto` 13 次，与预期一致。
- 发送错误：发往不可路由地址，每次失败的发送调用上报一次，与原实现次数相同；关闭 lo 校验和卸载（`ethtool -K lo tx off`）后 GSO 仍成功，`EIO` 在 lo 上无法复现，其回退与 `EINVAL` 共用代码路径。
- 背压：20 Mbit/s tbf 下 2000 帧×8 包，高水位拒绝新包，已接受的数据报全部按序、完整到达，无错误回调；带队列 shutdown 正常。tbf 在 lo 上只在有新包入队时才放出最后排队的包，检查在等待期间从另一 socket 发 1 字节触发；未加触发时 HEAD 原始 transport 同样有约 60 个尾包滞留（965/1022），不是回归。

### 5.2 GSO 在生产中实际分段（`gso_trace.sh`，`jobs/u13-gso-proof.sh`，10 s）

| 负载 | 版本 | `udp_sendmsg` 次数 | 超过 1500 B 的发送 | `__udp_gso_segment` | 送达数据报 | 发送字节 |
| --- | --- | --- | --- | --- | --- | --- |
| GB 4000 | base | 11.01 M | 0 | 0 | 10.93 M | 10.98 GB |
| GB 4000 | gso2 | 3.12 M（−72%） | 1.19 M | 1.19 M | 10.88 M | 10.92 GB |
| WHEP 1536 | base | 6.98 M | 0 | 0 | 7.05 M | 4.30 GB |
| WHEP 1536 | gso2 | 4.45 M（−36%） | 0.72 M | 2.05 M | 7.08 M | 4.32 GB |

送达数据报（含服务端自身接收）与字节两版本一致；GB 平均每个 GSO 批次约 7.5 段，WHEP 约 2.2 段（音频包与短末包单独发送）。客户端 `whep_fanout`/`gb_fanout` 的解密、序号与进度检查全部通过。

## 6. 固定 8 核 A/B 与容量

所有运行同一主机、同一 netns、同一 affinity、同一素材和 60 s 窗口；u09 按 base、gso2、gso2、base、base、gso2 交替。Gcyc/s、Ginstr/s 为 `perf stat -a -C 0-7`（包含 softirq）；schedutil 下负载下降会降频，busy 核数随之被拉长（GB gso2 busy MHz 1589→930），因此以 cycles/instructions 和容量边界为主要依据，busy 核只作参考。

### 6.1 同档 A/B（`jobs/u09-ab.sh`，每组 3 次；D 另加 `jobs/u12-d-repeat.sh` 4 次，`ab_sum.py`）

| 负载 | n base/gso2 | Gcyc/s base | Gcyc/s gso2 | 变化 | Ginstr/s | system 核 | softirq 核 | PSS MiB | 控制 p99 ms |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| GB UDP 4000 | 3/3 | 9.63（9.61–9.65） | 5.18（5.14–5.22） | **−46.2%** | 10.82→5.74（−46.9%） | 3.71→2.33 | 1.55→2.26 | 92→91 | 123.9→54.6 |
| WHEP 1536 | 3/3 | 8.41（8.35–8.50） | 7.37（7.32–7.43） | **−12.4%** | 11.02→10.00（−9.2%） | 2.51→2.05 | 1.04→1.31 | 433→433 | 10.1→7.5 |
| D 2.2× | 7/5 | 17.91（17.83–18.01） | 16.90（16.84–16.96） | **−5.6%** | 24.10→22.85（−5.2%） | 1.97→1.48 | 0.86→0.94 | 1121→1120 | 9.0→7.3 |
| A 1.0× | 3/3 | 16.37（16.32–16.42） | 15.73（15.70–15.78） | −3.9% | 16.01→15.16（−5.3%） | 2.11→1.90 | 1.29→1.33 | 1455→1444 | 12.9→15.2 |
| B 1.6× | 3/3 | 17.65（17.50–17.80） | 17.21（17.18–17.23） | −2.4% | 22.42→21.78（−2.9%） | 1.70→1.46 | 0.87→0.92 | 779→777 | 6.9→6.5 |
| C 2.15× | 3/3 | 17.35（17.32–17.37） | 17.20（17.17–17.21） | −0.9% | 23.62→23.37（−1.0%） | 1.11→1.03 | 0.55→0.56 | 1308→1310 | 14.3→12.2 |

- 每组内部范围不重叠，变化方向与归因一致：UDP 发送占比越高收益越大（GB 83%→−46%，C 6%→−1%）。softirq 上升是 lo 上 GSO 分段与交付移到 softirq 统计，总 cycles 仍下降。
- 每数据报 CPU（u09 Gcyc/s ÷ u13 送达数据报速率）：GB 约 8.8k→4.8k cycles；WHEP 约 11.9k→10.4k。
- realtime 比全部 ≥0.9989；GB 积压延迟列（lag）base 174–226 ms、gso2 122–134 ms；WHEP 25–31→11–15 ms；`RcvbufErrors` 增量、`queue_full`、`no_buffer_space` 均为 0（C 2.15× base r3 一次 572 个 UDP 丢包，SERVER_UNSTABLE，属饱和边缘，gso2 3/3 无丢包）。
- Worker 负载 max/min 不变（例如 D 0.92/0.89→0.87/0.85），FD 峰值相同，清理后 fd 均回到 32。
- A/C 列 `PASS(gb-probe)`：与上轮相同的探针/素材不兼容（见 7.4），其余检查全部通过。

### 6.2 健康容量边界（`jobs/u10-boundary.sh`、`u14-boundary2.sh`、`u16-boundary-ac.sh`）

| 负载 | base | gso2 | 判定 |
| --- | --- | --- | --- |
| D | 2.2× PASS（7/7）；2.4× FAIL 2/2（丢包 12908 / 32732） | 2.4× PASS 2/2（busiest 0.93–0.94，无丢包）；2.6× FAIL 2/2 | 健康容量 **2.2×→2.4×（+9%）** |
| B | 1.6× PASS；1.7× FAIL 2/2（丢包 10439 / 267）；1.8× FAIL 2/2 | 1.7× PASS 2/2（busiest 0.95–0.96）；1.8× FAIL 2/2 | **1.6×→1.7×（+6%）**，1.7× 余量小 |
| A | 1.0× PASS 3/3；1.05× PASS(gb-probe) 1/1（busiest 0.85，上轮同档 2/2 有 13–23 个丢包，处在边缘） | 1.0× PASS 3/3；1.05× PASS(gb-probe) 1/1（busiest 0.82，Gcyc/s −3.9%） | 不变（单次运行无法区分边缘档，不声称提高） |
| C | 2.15× 服务端健康 2/3（r3 一次 572 丢包）；2.3× FAIL（丢包 149919） | 2.15× 服务端健康 3/3；2.3× FAIL（丢包 103244） | 不变 |
| GB UDP | 7000 路 PASS 2/2，16.7 Gcyc/s，busiest 0.82 | 7000 路 PASS 2/2，8.8 Gcyc/s，busiest 0.73 | 边界 INCONCLUSIVE：10000 路时测试客户端 `EADDRNOTAVAIL`（临时端口耗尽），属 harness 限制；同档 cycles −47% |
| WHEP | 2048 / 2560 路 PASS | 2048 / 2560 路 PASS | 边界未到达（客户端约束）；同档 cycles 2048 −13%、2560 −5% |

参考健康档 A 1.0×、B 1.6×、C 2.15×、D 2.2× 在 base 上全部复现。

## 7. 协议、Sanitizer、生命周期检查（候选二进制，`jobs/u07-verify.sh`、`u11-gbprobe.sh`、`u17-tsan-d.sh`）

### 7.1 协议矩阵

| 构建 | 场景 | 结果 |
| --- | --- | --- |
| Release | 35 | 32 PASS，3 XFAIL（`rtsp-tcp-h264-aac`、`rtsp-udp-h264-aac`、`rtsp-tcp-h265-aac`，RTSP AAC 锯齿，严格 XFAIL/XPASS 规则未改） |
| ASan+UBSan | 22 | 20 PASS，2 XFAIL；30 份 sanitizer 日志只有 swapcontext 警告 |
| TSan | 12 | 11 PASS，1 XFAIL；0 报告 |

矩阵覆盖 WHIP/WHEP（含 Chrome offer、纯视频）SRTP 解密、GB UDP 与 TCP 主被动的 RTP/RTCP 与 PS 音画解码、多源跨 worker、慢读者、源停止和无效/重放 token。

### 7.2 Chrome WHEP 边界（`whep_boundary.py`）

Release、ASan、TSan 各一次，全部 PASS：whip 与主动 Pull 两个源 × Chrome 仅视频 / PCMU / 音视频 offer，1280x720 30 fps，freeze 0、丢帧 0、丢包 0；DELETE 204 且 1 s 内会话关闭；源结束后 receiver 与 fd 回收。ASan 1 份日志只有警告。

### 7.3 生命周期 churn

| 运行 | 构建 | 结果 |
| --- | --- | --- |
| WHEP：`whip:8,whep_pull:8` → `whep_video:32,whep:16,flv:4`，churn whep_video:16、源 churn（SIGKILL/SIGTERM 交替） | Release / ASan / TSan | PASS ×3；realtime ≥0.9994；udp drops 0；清理后无残留 receiver |
| GB：`rtmp:8` → `gb_udp:256,whep:16,flv:4`，churn gb_udp:64、源 churn | Release / ASan / TSan | PASS ×3；fd 峰值 720–721，清理后回到基线 |
| 混合 D 0.25×（AAC→Opus egress 路径），churn whep:16、源 churn | TSan 候选 / TSan HEAD | PASS ×2（结果目录 `u15-tsan-d-*`）；160 纯视频 + 64 音视频 WHEP、FLV/RTMP/HLS 全部就绪，churn 3 轮 0 失败，realtime ≥0.9993；TSan 0 报告，SRTP protect 失败 0 |

sanitizer 报告：ASan 只有 swapcontext 警告，TSan 0。写失败（不可路由）、背压与缓冲区满、`EAGAIN` 部分回退、带队列 shutdown 由 5.1 覆盖；8 核运行中 `queue_full`/`no_buffer_space` 计数为 0。

### 7.4 GB 混合探针与循环素材

上轮报告的 `PASS(gb-probe)`：负载素材 `perf_fixture_720p30` 没有闪白/蜂鸣同步标记，且 `-stream_loop` 循环接缝处音频有 33 ms 缺口（`ffmpeg -stream_loop 2 -c copy` 复现：2 处 33.0 ms 不连续），GB 探针要求同步标记且不容许缺口。矩阵素材 `avsync_720p30` 编码参数相同（1280x720 30 fps Baseline 2 Mbit/s、AAC LC 128k 48 kHz），循环无缝（同样复现 0 处不连续）并带标记。

最小修正：不改 harness 判据，混合 A 1.0×、C 2.15× 改用 avsync 素材各跑一次（base、gso2）：4/4 完整 PASS（不是 `PASS(gb-probe)`）。GB UDP 41/39 个 A/V 标记、偏差 p50 18.3 ms（A，两版本完全相同）/ 1.3 ms（C），GB TCP active 39 个、p50 1.0 ms，音频缺口 0，参考素材逐帧比对通过；FLV/RTMP/HLS/RTSP/WHEP 同时通过。avsync 素材内容不同，负载较轻（C 2.15× 14.9 vs 17.3 Gcyc/s），所以容量比较仍用原素材，avsync 只用于完整音画验证。

## 8. CPU、PSS、延迟、控制面变化

- CPU：见 6.1；每数据报 GB −46%，WHEP −13%，混合 −1%～−6%，与 UDP 发送占比一致。
- PSS：所有负载 ±1%（暂存缓冲每 worker 线程一份，约 63 KiB，复用不释放）。第一版每帧 reserve 63 KiB 时 PSS 也无增长，但 CPU 回退，已废弃。
- 延迟：同一帧的包仍在同一 handler 内发出，不经定时器或额外 post；首包从“立即发送”变为“帧末或批满时发送”，推迟量等于打包剩余包所用时间（同步 CPU 时间，微秒级，隔离实验 8×1 布局 p50 +10–25 µs）。积压延迟列下降（GB 174–226→122–134 ms，WHEP 25–31→11–15 ms）。
- 控制面 p99：GB 4000 116.6–128.8→31.3–57.5 ms，其余负载在噪声范围内（5.9–15.2 ms）。
- 解码与音画：矩阵、浏览器、avsync 混合探针、WHEP av_probe 全部通过；WHEP 回放音画偏差 0.7–3 ms，两版本一致。

## 9. 未解决问题与部署适用范围

1. **WHEP SRTP `replay_fail`（与本改动无关，未修复）**：u09 中 gso2 D 2.2× 有 2 次运行各有一个 WHEP 会话在 SRTP 启动后 150–300 ms 内 `srtp_protect` 失败（libsrtp status 9 = replay_fail，同一 SSRC/序号被加密两次），会话关闭导致该组观众就绪超时，运行判 FAIL（其后的 `/receivers` 连接复位来自清理阶段）。证据：
   - 同一错误已出现在 10-30 的两次 ASan 运行（`63f180d`、`6fe3c5f`，均早于本 Goal），日志同样是 `audio rtp packetize failed codec opus` 后关闭；
   - SRTP 加密在 `udp_transport::write` 之前、由打包器回调执行，cork/GSO 不改变序号、SSRC 或加密调用；
   - 重复统计：D 负载 base 9 次 0 例，gso2 11 次 2 例（Fisher 精确检验双侧 p=0.48，不显著）；其余全部 u06–u16 运行 0 例。
   - 根因未确定；嫌疑路径为 WHEP 音频（AAC→Opus egress / 伴随流）在会话启动阶段的帧投递。TSan 在 D 0.25×（含 egress 与源 churn）上对候选和 HEAD 都没有报告，也未复现该错误（7.3）。建议另立任务以 debug 日志加 SSRC/序号定位，不在本 Goal 修改。
2. **lo 与真实网卡**：本轮客户端与 DUT 同 netns 走 lo，`tx: loopback delivery`（GB 25%）在真实部署中发生在客户端主机。真实网卡上 GSO 的分段在网卡（支持 USO）或发送端软件 GSO 完成；软件 GSO 时仍省掉每包的 syscall、路由、IP id、skb 分配和 socket 层开销，但分段本身留在 DUT 上。推断：真实网卡的收益比例与 lo 不同，需在目标网卡上复测；行为正确性不依赖网卡（不支持时 `EINVAL`/`EIO` 自动逐段回退）。
3. **GSO 不可用的环境**：经过 xfrm/IPsec、校验和卸载不可用时 `EIO`，段长超过路径 MTU 时 `EINVAL`，都回退到单包发送；回退时多一次失败的系统调用，没有对每个 socket 缓存“不支持”状态（按 Goal 不新增状态），如此类环境常见需再评估。
4. **GB UDP 与 WHEP 绝对容量边界**：客户端临时端口与客户端 CPU 先到达极限，未测出；同档 CPU 下降已确认。
5. **内核 `init_on_alloc`**：skb 清零约占 GB CPU 6%，属内核配置，未改。

## 10. GO / NO-GO

**GO**（待统一审查后提交）。GSO 帧内合并收益稳定、可归因、远超噪声（GB −46%、WHEP −12%、D −5.6%，各组范围不重叠），健康容量 D、B 各提高一档，A、C 不回退；数据报原子性、顺序、高水位、错误上报、关闭与生命周期语义经定向检查、矩阵、浏览器与 Sanitizer 验证。第 9.1 项 SRTP replay 为既有缺陷，需单独跟踪；第 9.2 项要求在真实网卡上复测收益。

## 附：文件

- `udp_trace.sh`、`udp_attr.py`、`trace_sum.py`：系统调用与调用栈归因。
- `udp_batch_bench.cpp`、`bench_sum.py`：隔离实验与汇总。
- `udp_transport_check.cpp`、`udp_transport_check.sh`：transport 定向检查（Release/ASan/TSan，strace、无校验和卸载、tbf 背压）。
- `gso_trace.sh`：生产负载中 GSO 分段与送达计数。
- `ab_sum.py`：A/B 汇总。
- `jobs/*.sh`：全部运行命令（u02 以 u04 名义运行；u06 为第一版候选，u08 为第一版回退定位；u15 首次因 churn 周期参数错误未运行，修正后以 u17 名义运行，结果目录仍为 `u15-tsan-d-*`）。`udp.sh` 依赖 [20261103-full-acceptance](../20261103-full-acceptance/report.md) 的 `mix.sh`、`lib.sh`、`life_summary.py`、`mix_table.py`、`whep_boundary.py`。原始结果、profile、bpftrace 输出在测试机 `runs8/`。
