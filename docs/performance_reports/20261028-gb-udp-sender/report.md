# GB28181 UDP Sender 资源与协议职责审查（2026-10-28）

## 0. 结论

**有一项可以从程序自身消除的开销：UDP sender 的 RTCP socket 从不读取。** 接收端按 RFC 3550 惯例向"RTP 源端口 + 1"回送 RR，本项目自己的 GB UDP receiver 也是每秒一次。这些 RR 会一直积压在内核接收队列里，直到填满 rcvbuf。之后每个 RR 都被内核丢弃，并计入 UDP 接收错误。

- **影响**：
  - 每个 sender 约 222 s 后长期占住 213 KB 内核内存。8000 路合计 1.59 GiB，不计入进程 PSS。
  - 全机 `RcvbufErrors` 按每路每秒 1 个持续增长。真实媒体输入的丢包因此被淹没，perf_run 在媒体完全实时的情况下也把所有 BASE 轮次判为 FAIL（SERVER_UNSTABLE）。
  - 内存较小的机器上，这部分内存会越过内核 `udp_mem` 压力阈值，影响全机的 UDP 接收（推断，第 5 节）。
- **最小修改**（`gb28181_udp_sender_session.*`，+22 行，`docs/protocols.md` 1 段）：
  - sender 在 RTCP socket 上常驻一个读循环，读出后丢弃。语义与原来"不处理反馈"相同。
  - 不改端口、SDP、SR 发送和会话所有权。
  - 关闭沿用现有路径：transport 关闭使读操作返回，循环看到终态后退出。
- **代价**：
  - 每个 sender 多一个协程，PSS 约多 8.8 KB，7500 路时多约 66 MB。
  - 服务端 CPU：13 组 A/B 中 12 组新版本略高，中位 +0.9%（−0.2%～+2.8%），Gcycles 中位也是 +0.9%。有没有 RR 差别不明显，原因没有单独定位。

| 项目 | 结论 | 提交 | 依据 |
| --- | --- | --- | --- |
| R1：RTCP socket 常驻读取并丢弃 | **ACCEPT** | `cb03380` | RR 场景 1000/5000/7500/8000 路：BASE 全部 FAIL，RTCP 队列 203 MiB～1.59 GiB、丢包 8.3 万～87 万；新版本全部 PASS，队列约 0、丢包 0。媒体、SR、控制面不变 |
| R2：没配远端 RTCP 时不建 RTCP socket | **REJECT** | — | 接收端照样会向端口 + 1 回 RR，没有 socket 时内核会回 ICMP 端口不可达，对端如何处理无法验证；端口池仍按对分配，省不出端口；每个空 socket 只占约 1.6～2.0 KiB 内核内存 |
| R3：RTP socket 也读取并丢弃 | **REJECT** | — | GB 接收端正常不向 sender 的 RTP 端口发数据；为不符合协议的对端再给每个 sender 加一个协程（约 8.8 KB）不划算。最坏情况仍受 rcvbuf 限制，两版行为相同 |
| R4：把 rcvbuf 压到极小、`shutdown(SHUT_RD)`、定期清空 | **REJECT** | — | rcvbuf 最小为 2304，之后每个 RR 都计为丢包；未 connect 的 UDP 做 SHUT_RD 返回 ENOTCONN，数据照常入队（本机实测）；定期清空需要额外定时器，而且仍会在间隔内积压和丢包 |
| R5：sender socket `connect()` | **未实施** | — | 只能过滤第三方地址，挡不住对端自己回的 RR；还会让 ICMP 端口不可达在发送时变成 ECONNREFUSED，改变"对端停止接收"时的关闭语义。属于 891aec1 报告 B2 的独立方向 |

最终回答：**GB28181 UDP Sender 确实存在可以由程序自身消除的队列开销，就是 RTCP socket 不读取造成的内核接收队列积压和持续丢包计数。** 消除方法是在已有的 RTCP socket 上读取并丢弃，不改变端口约定、SR、SDP，也不依赖任何系统参数。其余 socket 资源都属于协议约定或部署契约，保留。

## 1. 版本与状态

| 项目 | 值 |
| --- | --- |
| BASE_HEAD | `83bf251`（开始时 `main` = `origin/main`，工作区干净） |
| 测试工具提交 | `f41d318` GB UDP 性能客户端模拟对端 RTCP 行为 |
| 生产修改提交 | `cb03380` 国标 UDP sender 读出并丢弃对端 RTCP |
| FINAL_HEAD | `cb03380`（`main` 领先 `origin/main` 2 个提交，工作区干净；报告目录被 Git 忽略） |
| BASE ELF（`rt-gbA`） | `e50918bddcb4230c3138ecb29f718129791d5d04e2a621eef5e0790aed83d91a` |
| FINAL ELF（`rt-gbB`） | `89f841b3f6c5f13955799ad4f5018bf3e72f1bd894efd61b139f2a7bb4679294` |
| 测试客户端 | 两个包共用同一 `gb_fanout`（`108b3085…`）和 `perf_run.py`。包用 `rt-wb` 硬链接复制，只替换上述文件 |
| sanitizer 构建 | TSan `29b0f003…`、ASan `2b10bb11…`、UBSan `47af0639…`（当前工作树） |
| 测试机 | <TEST_HOST>，Xeon Silver 4216，服务端 CPU 0-7，客户端 8-15、24-31，同一 netns 回环 |

没有 push。

## 2. RTP/RTCP 职责

### 2.1 代码路径

- **创建**：`/gb28181/sender/create` 由 `handle_sender_create`（`media/http/gb28181_http.cpp:150`）构造 `gb28181_udp_sender_session` 后调用 `startup()`。
  - 成功时返回空 201，**不返回本地端口**。
  - Go 信令不调用这个接口（`signaling/` 中没有 `sender/create`），调用方只有外部控制方和 bench。
- **startup**：
  - 从端口池取一个偶数端口 P；RTP socket 绑定 `bind_address:P`，RTCP socket 绑定 `bind_address:P+1`，rcvbuf 均为系统默认。
  - 只有请求带 `remote_rtcp_port` 时才创建 librtp 上下文（`rtp_create`）。
  - 媒体由 `gb28181_rtp_sender` 从共享 PS 输出打包，经 `send_packet` 写 RTP socket。
- **SR**：只在配置了远端 RTCP 时启用。发出首个 RTP 包后开始，每 25 s 一次（`rtcp_reporting_started_` 是一次性事实），`safe_shutdown` 取消定时器。
  - SR 的 SSRC 等于请求中的 ssrc，包数和字节数由 `rtp_onsend` 累计。
  - 不发 BYE，会话结束由 SIP 层负责。
- **接收**：BASE 两个 socket 都没有读循环。RR、BYE 等对端 RTCP 从不被处理，这本身就是现有语义：sender 不根据反馈调整发送，也不做重传或速率控制。

### 2.2 两个本地端口的必要性

- **RTP 端口必须显式绑定**：
  - 源地址必须是配置的媒体 IP（`bind_address`）。
  - 端口来自端口池，而不是系统临时端口。本机和测试机的 `ip_local_port_range` 都是 32768–60999，与池的 49152–65535 重叠；用 0 端口绑定会占用池里的端口，导致其他会话 bind 失败，而 D12 规定池不扫描重试。
  - 现在调用方拿不到这个端口，SDP 中无法写出真实的发送端口（见第 14 节）。
- **RTCP 端口 P+1**：RFC 3550 §11 约定 RTP 用偶数端口、RTCP 用相邻的奇数端口，SDP 不写 `a=rtcp` 时接收端按 P+1 推出 RTCP 地址。
  - 本项目的 GB UDP receiver 在没学到 RTCP 地址时，就向 RTP 源端口 + 1 每秒发一个 RR（`gb28181_udp_receiver_session.cpp:21`、`:183`）。
  - 因此**不论 sender 是否配置 `remote_rtcp_port`**，遵循惯例的接收端都会往 P+1 回 RR。
  - 端口池本来就按对分配，所以 P+1 的 socket 不额外占用端口容量。
- **到达各端口的数据**：

  | 端口 | 正常协议下到达的数据 | 需要处理吗 |
  | --- | --- | --- |
  | P（RTP） | 无（GB 不用 rtcp-mux） | 不需要 |
  | P+1（RTCP） | 接收端 RR/SDES，常见 1 Hz～5 s 一次 | 内容不需要，但必须读走，否则积压 |

### 2.3 各场景下的 socket 必要性

| 场景 | RTP socket | RTCP socket | SR |
| --- | --- | --- | --- |
| 不配置远端 RTCP，对端不回包 | 必需 | 只用于占住 P+1，空闲 | 不发 |
| 不配置远端 RTCP，对端按惯例回 RR（本项目 receiver、常见实现） | 必需 | 接收 RR | 不发 |
| 配置远端 RTCP，对端不回包 | 必需 | 发 SR | 每 25 s |
| 配置远端 RTCP，对端回 RR | 必需 | 发 SR，接收 RR | 每 25 s |

没有发现"建了从来不用"的资源。不配置远端 RTCP 时，RTCP socket 不发送，但仍承接接收端的 RR。不建它就会让 RR 引发 ICMP 端口不可达，见 R2。

## 3. 生命周期与所有权（审查结论）

- **启动失败**：池耗尽、RTP 或 RTCP bind 失败、订阅失败、`rtp_create` 失败或 sender 启动失败时，`startup` 返回 false，HTTP 层调用 `shutdown()`。`safe_shutdown` 在所属 worker 上移出 `stream_`，然后依次注销 registry、取消定时器、关闭子 sender 和两个 transport、归还端口、销毁 librtp。部分创建成功的资源也只走这一条路径，只释放一次。
- **新读循环**：
  - 在 `startup` 成功的最后一步 spawn，捕获 `self`。
  - transport 关闭后读操作返回 `operation_aborted`，循环先检查 `stream_`，此时已为空，于是退出。
  - worker 停止时，取消信号会让读操作出错，此时 `stream_` 还在，循环调用 `shutdown()`，与 shutdown 订阅的效果相同，`safe_shutdown` 可以重复调用。
  - 未 connect 的 UDP socket 不报告 ICMP 错误，读操作在正常运行中不会出错。
- **发送失败**：入队失败只记日志，同步发送错误由 transport 投递给 error callback，触发 shutdown，两者都不变。
- **对端停止接收**：未 connect 的 socket 忽略 ICMP。sender 继续发送，直到被删除或源结束，这是现有语义（会话由 SIP 层管理）。
- **源结束**：`on_end` 触发 shutdown，sender 自动注销，之后的 delete 返回 404。
- **并发关闭与迟到回调**：所有操作都在所属 worker 上执行；定时器、读循环和 PS 回调都以 `stream_` 为终态守卫。
- **端口复用**：同一进程内由池保证不重复分配。归还后立即复用（实测 240 次创建只用了 8 个端口），没有 bind 冲突。

## 4. 资源开销的组成

| 资源 | 每个 sender | 说明 |
| --- | --- | --- |
| socket / FD | 2 / 2 | 7500 路 FD 15035，两版相同 |
| 空闲 socket 内核内存 | 约 1.6～2.0 KiB/个 | 本机绑定 8000 个 UDP socket 的 Slab 增量 |
| RTCP 接收队列（BASE，有 RR） | 增长到 213120 B 后不再下降 | 每个 52 B 的 RR 在回环上占 960 B（skb truesize），1 Hz 时约 222 s 填满 |
| 发送队列 | 约 0 | 非阻塞直接发送，`t=0`，没有入队拒绝 |
| 定时器 | 0 或 1 | 只有 SR |
| 协程 | BASE 0；新版本 1 | 新版本 PSS 每路约 +8.8 KB |
| 进程 PSS | BASE 约 7.7 KB/路 | 7500 路 BASE 57～63 MB，新版本 123～129 MB |
| CPU | 内核发送为主 | 891aec1 报告：内核占 91%，用户态不到 9% |

## 5. 接收队列问题的真实影响

`gbs.sh` 每 30 s 抓取 netns 内 `ss -uanmp`、`/proc/net/snmp` 和 `/proc/net/sockstat`。8000 路 `rr` 的 BASE 轮次（`gb-s-rr-8000-A-r1`）：

| 窗口内时间 | RTCP 队列合计 | 单 socket 最大 | RTCP 丢包累计 |
| --- | --- | --- | --- |
| 0 s | 150 MiB | 39360 | 0 |
| 90 s | 832 MiB | 128640 | 0 |
| 180 s | 1.48 GiB | 213120（满） | 3362 |
| 300 s | 1.59 GiB | 213120 | 874200 |

- **内核内存**：
  - 队列总量与 sender 数成正比：1000 路 203 MiB，5000 路 1016 MiB，7500 路 1.49 GiB，8000 路 1.59 GiB。
  - 全机 UDP 内存（`sockstat` 的 mem）BASE 为 53.8k～55.2k、270k、407k～413k、434k 页；新版本为 1.8k～3.2k、11.5k、17.5k～60.5k、16.5k 页，其中包括客户端 socket 的瞬时缓冲。
  - 这部分内存属于内核，不计入进程 PSS，所以 BASE 的 PSS 看起来更低。
- **丢包计数**：
  - 填满后，每路每秒丢 1 个 RR，netns 的 `RcvbufErrors`/`InErrors` 300 s 内增加 8.3 万（1000 路）到 87.6 万（8000 路），perf_run 的服务端 `udp_drops` 同步增长。
  - BASE 的 rr、rtcp_rr 轮次因此全部判 FAIL（SERVER_UNSTABLE），而媒体实时比例都是 1.000。
  - 运维上，用这个计数判断媒体输入丢包的方法失效。
- **跨机器风险（推断，未在测试机复现）**：
  - 内核默认 `udp_mem` 压力阈值约为内存的 1/8。测试机 93 GB 时为 3067398 页（11.7 GiB），与此相符。
  - 8 GB 机器约 1 GiB，16 GB 机器约 2 GiB。
  - 总量越过压力阈值后，内核只给每个 socket 保证 `udp_rmem_min`（4096 B），GB/RTSP/WHIP 媒体输入 socket 的突发会被丢弃。
  - 按实测每路 213 KB 计，8 GB 机器约 5000 路 sender、16 GB 机器约 1 万路就会越过阈值。测试机内存大，也不能修改全局 sysctl，所以没有复现这个后果。
- **不是内存泄漏**：每个 socket 的队列上限就是 rcvbuf。删除 sender 后 socket 关闭，内存随之释放（churn 与清理后 socket 数回到 0）。问题在于它长期占用内存并持续产生丢包。

## 6. 不同负载下的 CPU、PSS、FD 和内核内存

固定 8 核、8 个 worker、单源 720p30 2 Mbps + AAC，ramp 200/s，预热 15 s。BASE 为 A，新版本为 B。RR 和 noise 场景每秒每路 1 个数据报。

| 轮次 | 判定 A / B | 服务端核 A / B | Gcyc/s A / B | PSS A / B | FD | RTCP 队列 A / B | 丢包 A / B |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1000 quiet（300 s） | PASS / PASS | 2.413 / 2.416 | 2.10 / 2.09 | 28 / 36 MB | 2034 | 0 / 0 | 0 / 0 |
| 1000 rtcp（对端不回包） | PASS / PASS | 2.442 / 2.484 | 2.12 / 2.16 | 29 / 37 MB | 2034 | 0 / 0 | 0 / 0 |
| 1000 rtcp_rr | FAIL / PASS | 2.463 / 2.494 | 2.13 / 2.16 | 29 / 37 MB | 2034 | 203 MiB / 0 | 83406 / 0 |
| 1000 rr | FAIL / PASS | 2.430 / 2.451 | 2.10 / 2.12 | 28 / 36 MB | 2034 | 203 MiB / 0 | 83408 / 0 |
| 1000 rtp_noise | FAIL / FAIL | 2.416 / 2.435 | 2.09 / 2.11 | 28 / 36 MB | 2034 | RTP 队列 203 MiB / 203 MiB | 83402 / 83380 |
| 5000 rr | FAIL / PASS | 5.548 / 5.702 | 12.11 / 12.32 | 45 / 88 MB | 10035 | 1016 MiB / 0.5 MiB | 486762 / 0 |
| 7500 rr r1 | FAIL / PASS | 7.094 / 7.204 | 19.10 / 19.27 | 58 / 124 MB | 15035 | 1.49 GiB / 0 | 801056 / 0 |
| 7500 rr r2 | FAIL / PASS | 7.041 / 7.148 | 18.92 / 19.12 | 58 / 124 MB | 15035 | 1.49 GiB / 0.5 MiB | 807814 / 0 |
| 7500 rtcp_rr | FAIL / PASS | 7.272 / 7.359 | 19.58 / 19.73 | 63 / 129 MB | 15035 | 1.49 GiB / 0.2 MiB | 807141 / 0 |
| 8000 rr | FAIL / PASS | 7.487 / 7.554 | 20.19 / 20.30 | 61 / 132 MB | 16035 | 1.59 GiB / 1.3 MiB | 874200 / 0 |
| 7500 quiet r1（120 s） | PASS / PASS | 7.009 / 7.057 | 19.06 / 19.19 | 57 / 124 MB | 15035 | 0 / 0 | 0 / 0 |
| 7500 quiet r2 | PASS / PASS | 7.086 / 7.072 | 19.26 / 19.22 | 58 / 123 MB | 15035 | 0 / 0 | 0 / 0 |
| 8000 quiet | PASS / PASS | 7.498 / 7.566 | 20.35 / 20.52 | 60 / 130 MB | 16035 | 0 / 0 | 0 / 0 |

- 新版本的 RTCP 队列"约 0"指抓取瞬间有数百个 socket 各有 1 个 RR（960 B）等待读取。
- 丢包列取 RTCP socket 的 `d` 计数。noise 行取 RTP socket 的 `d` 计数，属于 R3 的范围，两版相同。
- 新版本的 CPU 代价：
  - 13 组 A/B 中 12 组新版本服务端核数略高，相对差 −0.2%～+2.8%，中位 +0.9%；Gcycles 相对差 −0.5%～+1.9%，中位 +0.9%。
  - 有 RR 的轮次（每个 RR 多一次 `recvmsg`、一次协程切换）是 +0.9%～2.8%。没有 RR 的轮次（quiet、rtcp、rtp_noise）是 −0.2%～+1.7%，两者没有明显区别。
  - 空闲协程本身不会产生系统调用，差异可能来自更大的内存占用，也可能只是单轮波动，没有单独定位。按约 1% 计入代价。

## 7. 已考虑和否决的方案

见第 0 节表格 R2～R5。补充两点：

- **R2 不建 RTCP socket**：节省 FD 和约 2 KiB 内核内存/路，但 P+1 仍由池占着。代价是向遵循惯例的接收端回 ICMP 端口不可达（本项目 receiver 未 connect，会忽略；其他平台的处理方式没有条件验证）。而且原本由 RTCP 端口安静吸收的 RR，变成了每个都需要内核生成 ICMP 的工作。收益小，互通风险不可验证，不采用。
- **回调式读取代替协程**：可以把 PSS 代价从约 8.8 KB 降到 1 KB 以下，但需要给 `udp_transport` 增加第二种读接口。现有接收循环（GB receiver、WHIP、WHEP）都用 `worker_context::spawn` 加 `read(yield)`，为 7500 路约 66 MB 的用户态内存（换回约 1.5 GiB 内核内存）新增接口不划算。如果以后 sender 数量再上一个量级，可以重新评估。

## 8. 实际采用的最小修改

提交 `cb03380`，`media/gb28181/gb28181_udp_sender_session.{h,cpp}`：

```cpp
    worker_.spawn([self](boost::asio::yield_context yield) { self->run_rtcp(yield); });
...
void gb28181_udp_sender_session::run_rtcp(boost::asio::yield_context yield)
{
    std::array<std::uint8_t, 1500> buffer{};
    boost::system::error_code error;
    for (;;)
    {
        boost::asio::ip::udp::endpoint endpoint;
        rtcp_transport_->read(buffer, endpoint, yield, error);
        if (!stream_)
        {
            return;
        }
        if (error)
        {
            shutdown();
            return;
        }
    }
}
```

`docs/protocols.md` 增加一段：GB UDP sender 绑定相邻的 RTP/RTCP 端口，只在提供 `remote_rtcp_port` 时发送 SR；收到的对端报告读出后丢弃，不据此调整发送。

不新增状态、锁、配置或 socket 选项；不改 `udp_transport`、端口池、SDP 和 SR。

## 9. A/B 性能与资源对比

见第 6 节。交替顺序为：1000 路逐场景 A/B、B/A 交替；7500 rr 两轮分别为 A→B 和 B→A；7500 quiet 两轮分别为 A→B 和 B→A。

- **资源**：有 RR 时，内核内存从每路 213 KB 降到约 0，丢包计数从每路每秒 1 个降到 0；PSS 每路约 +8.8 KB；FD、socket 数、定时器数不变。
- **控制面**（create/delete p99，探测 p99，单位 ms）：

  | 轮次 | create p99 A / B | delete p99 A / B | 探测 p99 A / B |
  | --- | --- | --- | --- |
  | 7500 rr r1 | 259 / 582 | 415 / 353 | 436 / 434 |
  | 7500 rr r2 | 436 / 664 | 596 / 541 | 430 / 401 |
  | 7500 rtcp_rr | 687 / 606 | 267 / 1136 | 376 / 430 |
  | 8000 rr | 846 / 582 | 854 / 606 | 432 / 447 |
  | 7500 quiet r1 | 379 / 310 | 833 / 641 | 396 / 377 |
  | 7500 quiet r2 | 647 / 345 | 538 / 256 | 375 / 388 |
  | 8000 quiet | 422 / 461 | 1290 / 614 | 407 / 366 |

  两版互有高低，没有一致方向。7500/8000 已接近 8 核饱和（7.0～7.6 核），单轮 p99 波动较大。
- **高负载行为**：没有新的 UDP 丢包、发送错误或入队拒绝（`queue_full` 和 `no_buffer_space` 均为 0），也没有资源无法回收。

## 10. RTP/RTCP 协议与媒体正确性

- **RTP**：`gb_client` 逐包检查 V=2、PT、SSRC 和序列号连续性，逐帧检查 PS pack header 和 PTS 单调；任何异常都会使该 viewer 失败。所有轮次的全部 viewer 都 progressing，实时比例 ≥0.999。本机生命周期脚本另外逐包检查序列号：gaps=0，bad=0。
- **SR**：1000 路 300 s 两版都收到 12000 个 SR（每路 12 个，即每 25 s 一个），7500 路 rtcp_rr 两版分别为 96435 和 96171。每个 SR 都校验了 V=2、PT=200、SSRC 等于请求值、包数非 0，无效包为 0。
- **级联解码**（本机，`l-{base,drain}-cascade-20`）：上游 media_server 的 GB UDP sender 推给被测 media_server 的 GB UDP receiver，再出 FLV。
  - `--decode-probe` 两版都解出 H.264 1280×720（180 帧）和 AAC 48 kHz 双声道（280 帧）。
  - 上游 sender 的 RTCP 队列：BASE 在 120 s 时为 20 路共 700800 B（每秒每路 +960 B）；新版本始终为 0。这是用本项目自己的 receiver 作为真实对端的复现。
- **H.265 与 G.711**：本轮修改不涉及 PS 打包与 RTP 负载，没有重跑这两种编码。891aec1 报告中 H.265+AAC、H.264+G.711A 的 GB UDP 输出已通过。

## 11. 观看延迟、流畅性和控制响应

- **滞后跨度**（viewer lag span max，ms）：1000 路 220～230 / 228～230；7500 路 641、656、637 / 658、662、717；8000 路 680 / 658；quiet 7500 591、586 / 571、544。
  - 两版相同量级，7500 rtcp_rr 新版本单轮高 80 ms。
  - 高负载下的滞后跨度主要来自 8 核接近饱和后单批交付的时长。
- **流畅性**：所有轮次实时比例 0.999～1.000，没有 non-progressing viewer。
- **控制响应**：见第 9 节。

## 12. 并发、生命周期与异常关闭

**本机 `san/scripts/gb_lifecycle.sh`**：单个服务进程、4 worker，对端为 Python 收发端。BASE、新版本、TSan、ASan、UBSan 各跑一遍，结果一致，只有 RTCP 队列一项不同：

| 场景 | 结果 |
| --- | --- |
| 30 轮 × 8 个 sender（一半带 RTCP）创建、收流 3 s、删除，期间 1 Hz 回 RR | 240 次 201、240 次 204，全部收到媒体；只用了 8 个端口（立即复用）；结束后服务端 socket 0 |
| SR 定时器已启动时删除（4 路带 RTCP 收流 28 s） | 每路 1 个 SR，gaps 0；RTCP 队列 BASE 107520 B、新版本 0；删除 204，socket 回到 0 |
| 对端关闭接收 socket 10 s | 服务端继续持有 socket，发送队列 0，没有错误日志；删除 204 |
| 向 RTP/RTCP 端口发垃圾（每端口 54 个，1～65000 B） | 媒体不中断、无缺口；新版本把 RTCP 端口的垃圾读空（BASE 积压 888992 B） |
| 非法请求 | RTCP 端口与 RTP 端口相同返回 400；源不存在返回 500；重复 sender_id 返回 500；删除不存在的 sender 返回 404 |
| 源结束（推流端 SIGKILL） | 8 个 sender 自动结束，socket 回到 0，随后删除返回 404 |
| 带 8 个活跃 sender 和 RR 时 SIGTERM | exit 0，3～54 ms（TSan 最慢） |
| sanitizer 诊断 | 0（ASan 只有 ucontext 的固定提示） |

**本机 perf_run + sanitizer**（`pf-{tsan,asan}-gbrr100`）：100 路 `rr` + 每 15 s 一轮 20 路 churn，60 s。PASS，删除全部成功，诊断 0。

**测试机长稳**（`gb-l-rr-6400churn-*`）：见第 12.1 节。

### 12.1 长稳与 churn

`gb-l-rr-6400churn-{A,B}-r1`：6400 路 `rr`，加上每 20 s 一轮、每轮 100 路的 churn（也回 RR），窗口 900 s。

| 项目 | BASE | 新版本 |
| --- | --- | --- |
| 判定 | FAIL（SERVER_UNSTABLE） | PASS |
| churn | 43 轮，失败 0 | 43 轮，失败 0 |
| 实时比例最小 / 滞后跨度最大 | 1.000 / 579 ms | 1.000 / 622 ms |
| 服务端核 / Gcyc/s | 6.159 / 16.39 | 6.333 / 16.56 |
| PSS（0 s → 270 s → 570 s → 870 s） | 53.4 → 53.6 → 53.4 → 53.6 MB | 110.1 → 112.7 → 113.0 → 112.9 MB |
| FD | 12833～13035 | 12833～13033 |
| RTCP 队列（870 s） | 1.27 GiB（6500 个 socket 全满） | 0 |
| RTCP 丢包（窗口内） | 4395018 | 0 |
| create / delete p99 | 481 / 176 ms | 484 / 242 ms |
| 控制探测 p99 | 306 ms | 378 ms |

两版资源都没有漂移，churn 创建的 sender 全部回收。新版本控制探测 p99 高 72 ms，是单轮观察，第 9 节的 7 组对比没有一致方向。

## 13. 结论

- R1 **ACCEPT**：
  - 找到了明确的程序内部问题（读路径缺失导致内核队列长期占满并持续丢包）。
  - 修改后内核内存和丢包计数实际下降，在 1000/5000/7500/8000 路上可重复。
  - RTP、SR、媒体解码、观看体验和控制面没有退化；生命周期和异常清理通过 sanitizer 验证。
  - 实现 22 行，沿用现有读循环模式，不依赖任何全局内核配置。
  - 代价是每路约 8.8 KB PSS，服务端 CPU 约 +1%。
- R2、R3、R4 **REJECT**；R5 不在本轮范围（需单独评估错误语义）。

## 14. 后续

- **发送端口未返回**：`/gb28181/sender/create` 不返回本地 RTP 端口。若级联平台需要在 200 OK SDP 中写出真实发送端口，或依据 `a=rtcp`/P+1 回 RR，需要接口返回端口。这是接口能力问题，不是性能问题。
- **端口池容量**：每个 UDP sender 占一对端口，单机上限 8192 个（`media_port_pool.h`）。本轮 8000 路已接近这个上限，再往上属于部署配置问题。
- **B2 `connect()`**：891aec1 报告显示每包路由查找约占 4.4%，但会改变 ICMP 错误语义，需要先决定"对端端口不可达时是否停止 sender"。
- **B3 批量发送**（sendmmsg/GSO）：GB UDP 的主要成本在内核逐包发送，需要在真实网卡上评估。
- 除此之外，GB UDP sender 的 socket、队列和定时器没有其他值得处理的冗余。

## 附：文件

- `tools/`：
  - `gbab.py`：单轮汇总；
  - `sssum.py`：`ss` 汇总；
  - `local_run.sh`、`local_cascade.sh`：本机运行；
  - `sync.sh`：拉回运行数据，排除凭据；
  - `test-host/`：`gbs.sh` 以及 job 300～303。
- `raw/runs/`：测试机各轮结果、`*.gbs.log`（30 s 采样），以及 30 s/270 s 两份原始 `ss` 快照。
- `raw/local/`：本机各对端模式与级联轮次。
- `san/`：生命周期脚本、sanitizer 运行与 `runs/run_all.log`。
