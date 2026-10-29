## 10 现有架构下的优化点

先给出成本结构的上限，再讨论候选优化：

- **TCP 输出（RTMP/FLV/RTSP/GB TCP）**：服务端周期约 55%～60% 在内核网络栈（发送 33%～41%，回环接收 16%～21%），6%～15% 是缺页，用户态合计 25%～35%（第 7.2 节）。即使把用户态成本全部消除，容量最多提高约 1.4 倍；实际能消除的是写路径上的分配、拷贝和调度开销，约占 5%～15%。
- **UDP 输出**：GB UDP 的用户态不到 9%；WHEP 的用户态约 40%，其中 SRTP 占 19%。剩下的空间在内核逐包成本，只能靠减少系统调用和 skb 数量。
- **WHIP 输入**：约 2/3 的周期是 AAC 编码，主要靠调整编码参数。
- **饱和点附近的问题**：大于效率问题。F4、F5 让 TCP 输出在超过饱和点约 3% 时就集体断开、内存膨胀到 11～15 GiB，也让控制面在饱和时失去响应。

“已实施”指 1ac4e04→04334df 一轮已经通过 A/B 验证合入的优化（`25c8607` RTMP 按帧聚合、`ccd716b`/`1c92239` RTSP 按帧聚合与预留、`7a258f8` GB TCP 按事件批量、`b18d69f` HTTP-FLV 发送回调、`4abe523` GB UDP 包所有权移交，见 `docs/performance_reports/20261023-core-slimming/README.md`），本轮不再重复提出。

### 10.1 第一类：证据充分，值得优先实施

| # | 优化点 | 证据 | 预期收益 | 复杂度 | 正确性风险 | 维护成本 |
| --- | --- | --- | --- | --- | --- | --- |
| A1 | **`tcp_transport` 持有待发缓冲**：写入时直接追加到待发缓冲并立即检查 1 MiB 高水位；有写在途时只追加，完成后一次写出全部积压。drain 和 `write()` 已在 socket 所属 worker 上执行（`worker_sink_dispatcher.h:109`），可以去掉逐次 `post`、`make_shared` 和逐次 `async_write` | F5：RTMP 9000、RTSP 8250/8500、100 源 RTMP 8750/9000、混合 7750/8000 集体 EOF，PSS 11～15 GiB。GB TCP 用同样的批量方式，饱和后每 viewer 周期从 2.6 降到 1.55 Mcycles，13750 仍实时、无断开。写路径用户态开销：transport 5.4%、分配器 3.5%、shared_ptr 1.3%～3.4%；GB TCP 二次拷贝 memcpy 10%、缺页 12%～15% | 过载时：排队数据受高水位约束，PSS 上限约为 viewer 数 × 1 MiB，慢 viewer 逐个被关闭，不再整 worker 断开；饱和后按 GB TCP 的方式降级（延迟增大，不断流）。饱和点以下：每次写少一次分配和一次调度，估计节省 TCP 输出 5%～10% 的周期，GB TCP 去掉会话层缓冲后可能节省更多；需 A/B 确认 | 中：集中在 `tcp_transport` 内部，`write(span)` 接口不变；HTTP-FLV 使用自己的队列（`http_flv_session::enqueue`），需同样处理 | 中：`no_buffer_space` 回调仍需异步投递，避免在调用方栈内重入关闭；需保持 C05 的背压契约和写完成回调语义 | 低：减少了状态（不再有 post 中的、未计入高水位的数据） |
| A2 | **drain 分批让出**：`sink_group::drain()` 每次只处理一批（一次 swap），还有待处理帧就重新 post 自己后返回 | F4：GB UDP 8000 窗口结束时删除 sender 的请求 10 s 超时（只删掉 161～207/8000）；GB TCP active 14000 create 超时；WHEP 3750 饱和时新会话 STUN nomination 超时；在 F5 中，drain 不返回时同 worker 的写全部推迟 | 饱和点附近控制面、新会话建立和写完成能与分发交替执行；与 A1 一起消除 F5 的积压来源。饱和点以下吞吐不变 | 低：约 10 行，`drain_queued` 保证同组只有一个 drain，帧序不变 | 低 | 低 |
| A3 | **UDP 媒体输入设置 `SO_RCVBUF`**（GB UDP 接收、RTSP UDP 发布、WHIP、WHEP 拉流），建议 2～4 MiB | F6：GB UDP 300 路丢包 204（服务端 30% 繁忙，单核最高 0.35）；WHIP 200 路丢包 98783（周期只用到满载能力的 59%）；WHEP 拉流 100 路丢包 813；带采样时 WHIP 100 路、GB UDP 200 路也丢包。测试客户端早已为同样原因调大接收缓冲（`bench/clients/gb_client.cpp:200`、`webrtc_client.cpp:478`） | UDP 输入不再在 CPU 远未饱和时因关键帧突发丢包，GB UDP 输入可接入的源数和 WHIP 输入容量回到由 CPU 决定。机制是推断（附录 B F6），A/B 后才能确认 | 很低：`udp_transport` 接收 socket 一处 `set_option` | 低。设置会被 `net.core.rmem_max` 截断：本测试机为 2147483647，默认 Ubuntu 为 212992，部署文档需同时说明 sysctl | 低 |
| A4 | **WHIP/WHEP 拉流的 Opus→AAC 编码器参数**：现为 `avcodec_open2(encoder, codec, nullptr)`（`media/codec/audio_transcoder.cpp:206`），即默认 twoloop 量化器加 PNS/TNS/M-S/IS 全部工具；评估 `aac_coder=fast` 并关闭不必要的编码工具 | `in-whip-100` profile：AAC 编码约占 66%（twoloop 28.5%，band cost 18.9%，cbrtf 4.0%，心理声学/TNS/M-S 约 15%），Opus 解码 9.7%；WHIP 输入每源 134～141 Minstr/s，是 RTMP 输入的 24 倍；稳定容量 150 路 | 若 fast 量化器把编码成本降到原来的 1/3～1/2（需实测），WHIP 每源成本可降 30%～45%，输入容量相应提高；与 A3 一起实施才能体现在容量上 | 很低：编码器选项字典 | 中：音质变化，需在现有码率下做客观或主观对比 | 低 |
| A5 | **jemalloc `muzzy_decay_ms:10000`**：脏页衰减到期后先 `MADV_FREE` 而不是直接 `MADV_DONTNEED`。jemalloc 5.2.0 起默认关闭 muzzy 衰减（ChangeLog：“Disable muzzy decay by default”），10000 与 dirty 衰减时间相同。可以用部署环境变量 `MALLOC_CONF`，或在程序中定义 jemalloc 的 `malloc_conf` 全局符号 | 第 7.6 节 A/B：RTMP/FLV 5000 时缺页减少 50%～62%，每 viewer 指令数降 5%～7.4%；满载时 RTMP 8750 由 0/4 变为 3/3，RTSP 8250 由 0/2 变为 2/2，HTTP-FLV 8500 由 0/4 变为 2/3。关闭衰减（`-1`）不可行：PSS 无界上涨 | TCP 输出饱和点以下省 5%～7% 指令；稳定容量 RTMP +3%（8500→8750），RTSP 至少 +6%（7750→8250），HTTP-FLV 8500 进入边际。效果只针对写路径分配大的 TCP 输出，UDP 输出的 PSS 只有几十 MiB，预计无收益 | 很低：运行参数或一行全局定义 | 低：不改语义；PSS 多 190～240 MiB（5000 viewer），这些页在内存紧张时由内核回收 | 低 |

### 10.2 第二类：有潜在收益，需要补充实验

| # | 优化点 | 证据与现状 | 需要的实验 | 预期收益 | 复杂度与风险 |
| --- | --- | --- | --- | --- | --- |
| B2 | UDP sender `connect()` | GB UDP、WHEP 的 sender socket 未 connect，每包查路由：GB UDP 4.4%，WHEP 3.3%（`ip_route_output_flow`） | 固定负载 A/B；WHEP 在 nomination 更新端点时需重新 connect（D08） | 约 3%～4% | 低到中：connect 后 ICMP 错误会以 `ECONNREFUSED` 出现在后续发送上，错误语义需对齐 C05 |
| B3 | UDP 按帧批量发送（`sendmmsg` 或 UDP GSO） | GB UDP 内核占 91%，每 viewer 每秒 254 次 sendto，skb 分配 14.2%、系统调用进出 6.4%；WHEP 每 viewer 每秒约 410 次 | 先在真实网卡上测，回环上 GSO 的分段会挪到接收侧，测不出真实收益 | 系统调用进出和部分逐包开销，可能为 10%～30%，取决于网卡 | 中到高：transport 增加批量接口，需保持 datagram 丢包语义（D04、C05） |
| B4 | WHEP SRTP 每包开销 | libcrypto 约 10%，OpenSSL 3 每包重新初始化 cipher 的参数处理约 4.6%，`protect_rtp` 每包分配零初始化 vector 再拷贝 | libsrtp 版本或 OpenSSL 用法评估（`third/` 本轮不改）；`protect_rtp` 的缓冲复用可单独 A/B | 5%～7% | 中：涉及 `third/` 依赖或加密路径 |
| B5 | AAC→Opus 共享 egress 的编码复杂度 | 100 源 WHEP 时，每个有 WHEP viewer 的源固定约 75 Minstr/s（第 5.3 节），libopus 使用默认复杂度 | 先做 profile 确认 libopus 占比，再在可接受音质下 A/B 复杂度 | 多源 WHEP 场景每源约数十 Minstr/s | 低：编码参数；音质风险 |

### 10.3 第三类：收益较小，不值得引入复杂度

见第 12 节。

### 10.4 第四类：既定架构约束或业务特性，不为性能改变

| 项 | 性能影响 | 约束依据 |
| --- | --- | --- |
| 每个 viewer 独立的 RTMP chunk 流、RTP 打包、SSRC/sequence、SRTP 加密 | 封装占 4%～10%，WHEP SRTP 约 19% | D03：FLV tag、MPEG-PS、AAC→Opus 已按源共享，viewer 的 RTP 身份和加密状态必须独立；任务也明确不引入跨 viewer 共享 RTP |
| WHIP/WHEP 拉流的 Opus 无条件转 AAC | WHIP 输入每源 134～141 Minstr/s | `docs/protocols.md`：Opus 先转 AAC 再进入内部流，保证 RTMP/FLV/HLS/GB 输出可用。改为按需转码属于转码体系变化，本轮只建议在现有转码器内调参数（A4） |
| TCP 写高水位 1 MiB、UDP 丢新包、过载关闭由上层决定 | 决定过载行为 | C05、D04。A1 是让高水位真正生效，而不是放宽它 |
| 端口池只负责获取和归还（D12），容量 8192 对 | GB UDP 稳定容量 7500，已接近上限 | 属于配置项；更大规模需要调整端口范围，而不是改端口池机制 |
| 本测试机只有 3 个内存通道生效 | TCP 输出在容量点附近受访存限制 | 硬件配置，不是代码问题；部署时建议插满内存通道 |
| 8 核限制 | — | 本轮约束 |

### 10.5 扩展性

第 7.7 节：1～8 个 worker 下每 viewer 指令数不变，没有软件层面的扩展瓶颈；每 viewer 周期随核数上升，原因是共享 L3 和内存（3 通道）的竞争，RTMP 1→8 worker 时 +34%，GB TCP +65%。因此：

- 线程模型、锁和 worker 间协调不需要改（第 12 节）。
- 能提高 8 核容量的是减少每 viewer 的访存量和缓存占用：A1 去掉写路径的逐次分配和拷贝，A5 减少缺页清零；GB TCP 的会话层二次拷贝（memcpy 10%）也属于这一类，可在 A1 中一并处理。
- 在更多内存通道的主机上，同样 8 核的 TCP 输出容量可能更高（第 7.5 节），本轮无法验证。

## 11 候选优化的收益、复杂度与风险

按“预期收益 ÷（复杂度 + 风险 + 维护）”排序：

1. **A2 drain 分批让出**：改动最小，直接修复 F4，也是 F5 的前提条件之一。
2. **A3 UDP 输入接收缓冲**：改动最小，直接决定 UDP 输入的源数上限。需要同时给出部署 sysctl。
3. **A1 TCP 写路径**：收益最大（过载行为、内存上限、饱和后降级，并有一定的饱和点以下效率），复杂度中等，需要完整的慢 viewer、关闭、背压回归。
4. **A5 jemalloc muzzy 衰减**：零代码风险，已在容量点验证（RTMP +3%，RTSP 至少 +6%），可以立即作为部署参数使用；A1 实施后写路径分配减少，收益可能相应变小，届时复测。
5. **A4 AAC 编码参数**：改动极小，收益只在 WHIP/WHEP 拉流输入上，需要音质验证。
6. **B2～B5**：先做实验，再决定是否实施。

验收方法（下一轮 A/B，同一测试机、同一运行包、交替测量）：

- A1/A2：
  - RTMP/RTSP/GB TCP 在 3000 和 6000 viewer 下比较周期、指令、缺页和 PSS；
  - RTMP/RTSP 容量搜索，同时报告实时比例和 lag span；
  - RTMP 9000 过载轮不再集体 EOF，PSS 不超过 viewer 数 × 1 MiB；
  - GB UDP 8000 删除请求全部成功；
  - WHEP 3750 新会话建立不再超时。
- A3：`in-gb_udp-300`、`in-whip-200`、`in-whep_pull-100` 丢包为 0，并对比 CPU。
- A4：`in-whip-100/150/200` 的每源成本和容量，以及音质对比。

## 12 不值得继续优化的方向

仓库文档（`.agent/decisions.md`、`.agent/lessons.md`、`docs/performance.md` 和上一轮报告 `20261023-core-slimming`）没有记录此前测试后被否决的性能方向，下表结论都来自本轮数据。

| 方向 | 证据 | 结论 |
| --- | --- | --- |
| 锁、线程模型、调度 | futex 0～10 次/s；off-CPU 100% 是 `epoll_wait` 空闲；8 个 worker 负载均匀；上下文切换 0.7～1.6 k/s | 没有可消除的等待，不值得改 |
| io_uring | 系统调用进出（kother）只占 2%～6%，主要成本在协议栈逐包处理和数据拷贝，io_uring 不减少这些 | 复杂度高、收益小 |
| `MSG_ZEROCOPY` | 用户数据拷入 skb（`rep_movs_alternative`）最多 10%；TCP 输出多为几 KB 的小消息，零拷贝的页固定和完成通知开销更大，回环上还会退回拷贝 | 不值得 |
| 协议封装（RTMP chunk、RTSP/RTP 打包、FLV tag） | 占 4%～10% | 收益小 |
| 输入侧 H.264 扫描（`mpeg4_h264_annexb_nalu`、`mpeg_h264_find_new_access_unit`） | 占输入成本的 8%～22%，但输入只占服务端总成本的很小一部分（200 路约 1.7 Gcycles/s，满载能力的 8%）；代码在 `third/` | 不值得 |
| HLS 服务端 | 0.22 Minstr/viewer·s，14000 viewer 时 3.2 核 | 已经足够轻 |
| 回环接收 softirq | 16%～22%，测试环境产物 | 不是优化对象 |
| 关闭 jemalloc 衰减（`dirty_decay_ms:-1,muzzy_decay_ms:-1`） | 4 轮 PSS 都随 viewer 数无界上涨，warmup 中超过 16 GiB 预算（第 7.6 节） | 不可行；改用 A5 |
| 增加核心数、扩大队列 | 前者违反本轮约束；后者违反 C05，只会推迟 F5 并放大内存。扩展性测试也显示多加的核会被共享 L3 和内存竞争部分抵消（第 7.7 节） | 不做 |

## 13 最终结论

**边界**（8 核、单源 720p30 2 Mbps、jemalloc 默认参数，第 4 节）：

| 输出 | 稳定容量 | 先失败在哪里 |
| --- | --- | --- |
| RTMP / HTTP-FLV / RTSP TCP | 8500 / 8250 / 7750 | CPU 100% 后媒体落后，再多约 3% 就整 worker 断开（F5） |
| GB UDP | 7500 | 媒体仍实时，控制面请求超时（F4）；端口池 8192 对也已接近 |
| WHEP | 3250 | 新会话 ICE/DTLS 排不上（F4），已建立会话仍实时 |
| GB TCP active / passive | 13750 / ≥14000 | 9500～10000 起 CPU 饱和，合并写使媒体不丢，交付延迟随负载增长 |
| HLS | ≥14000 | 测试客户端先到上限，服务端只用 3.2 核 |
| 100 源 RTMP / 六协议混合 | 8500 / 7000 | 同单源，失败方式为 F5 |

**长稳**（第 8 节）：RTMP 7000、WHEP 3000、GB UDP 6400 各 30 min，带每 20 s 一轮的 churn，全部 PASS。CPU、PSS、FD 没有漂移，churn 会话全部回收，清理后 FD 回到基线；空闲后 PSS 由 jemalloc 保留不归还，属于分配器行为，不是泄漏。

输入侧，RTMP/RTSP/拉流每源 4～6 Minstr/s，几百路也只占很小一部分；WHIP 稳定 150 路，限制来自同步 Opus→AAC 转码和 UDP 接收缓冲（F6），不是总 CPU。GB UDP 输入 300 路就因 F6 丢包。

**首先遇到的瓶颈**：所有输出协议都是服务端 8 个核的 CPU。TCP 输出 55%～60% 的周期在内核网络栈，其余受访存限制（IPC 0.6，3 个内存通道，共享 L3 竞争使 8 worker 的每 viewer 周期比 1 worker 高 34%）。锁、调度、线程模型都不是瓶颈。UDP 输入在 CPU 远未饱和时先撞上 socket 接收缓冲。超过饱和点后没有平缓降级：F4 让控制面失去响应，F5 让 TCP viewer 整 worker 断开、PSS 膨胀到 11～15 GiB。

**值得继续投入**（第 10.1 节，按顺序）：

1. **A2 drain 分批让出**：约 10 行，修复 F4。
2. **A3 UDP 输入 `SO_RCVBUF`**：一处设置加部署 sysctl，修复 F6。
3. **A1 `tcp_transport` 持有待发缓冲**：让 1 MiB 高水位真正生效，消除 F5，并减少写路径分配和调度；收益最大，需要完整的背压回归。
4. **A5 jemalloc `muzzy_decay_ms:10000`**：零代码，已验证 RTMP 8500→8750、RTSP 7750→8250；可以立即作为部署参数。
5. **A4 AAC 编码参数**：只影响 WHIP/WHEP 拉流输入，需要音质验证。

前三项主要改善饱和边缘的行为和 UDP 输入的可靠性；其中 A1 预计还能让饱和点以下的 TCP 输出节省 5%～10% 的周期，实施后需要 A/B 确认。按成本结构，即使用户态全部消除，TCP 输出容量最多提高约 1.4 倍，所以不要期待单项代码优化让容量成倍提高。

**需要先补实验**：B2 UDP `connect()`（3%～4%）、B3 UDP 批量发送（需在真实网卡上测）、B4 SRTP 每包开销、B5 libopus 复杂度。

**不值得投入**（第 12 节）：锁与线程模型、io_uring、`MSG_ZEROCOPY`、协议封装、输入侧 H.264 扫描、HLS、关闭 jemalloc 衰减（PSS 无界增长）。

**应保持的约束**（第 10.4 节）：每 viewer 独立的 RTP/SRTP（D03）、Opus 无条件转 AAC、C05 的写高水位和丢包语义、端口池只获取和归还（D12）。

**功能缺陷**（附录 B）：F1 WHEP 拉流 FLV/RTMP 时间线半速（中高）；F2 HLS G.711 私有 stream type，通用播放器无法解码音频（中）；F3 观测与接口语义（低）；F4、F5 饱和边缘行为；F6 UDP 输入接收缓冲（多源或关键帧大的源会丢包，H.265 GB UDP 输入时 HLS 首分片 2/4 轮超时）。

**结论的适用范围**：
- 回环 netns 让服务端承担接收侧 softirq（约 20%）；
- 内存只有 3 个通道生效，客户端与服务端共享插槽内存带宽；
- 10-24 00:00～01:40 的 TCP 输出周期成本比之后高约 25%，原因未确定，RTMP/FLV 容量有约 ±12% 的时段不确定度；
- HLS、GB TCP passive 的上限受测试工具限制；
- WHIP H.265 输入没有测试工具，未验证。

