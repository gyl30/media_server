## 7 热点、调用路径与资源瓶颈

### 7.1 方法

`tools/prof2.sh` 在固定负载下采样，只作诊断，不计入容量。测量窗口开始 20 s 后依次执行：

1. `perf stat` 统计服务端 8 核 15 s，同时记录 IMC 读写带宽和 `/proc/softirqs`；
2. `perf record -a -C 0-7 -g -F 499` 采样 15 s；
3. 用 `syscount-bpfcc` 统计系统调用次数和耗时 10 s；
4. 用 `offcputime` 统计 off-CPU 时间 10 s。

调用栈分类用 `tools/stackcat.py`，汇总表用 `tools/proftable.py`，按符号统计包含占比用 `tools/stackshare.py`。原始数据在 `raw/prof2/<name>/`。

工具问题与修正：

- 测试机 `/usr/local` 下的同名 PyPI 包 `bcc` 遮住了系统 BPF 库。prof2.sh 只给这两个命令加 `PYTHONPATH=/usr/lib/python3/dist-packages`，不改全局环境。
- 内核 6.8 的 `finish_task_switch` 符号带 `.isra.0` 后缀，旧版 offcputime 无法挂 kprobe，改用报告目录里改了符号名的副本 `tools/offcputime.py`。
- 因此 `rtmp-3000`、`rtmp-6000` 两组没有 syscall 数据，`flv-3000`、`rtsp-3000`、`whep-1000` 没有 off-CPU 数据。RTMP 的 syscall 和 off-CPU 由 `rtmp-6000b` 补测；它与 `rtmp-6000` 的周期相差 0.6%，各类别占比相差不到 0.2 个百分点，说明 profile 可重复。
- 采样本身有开销：`p2-in-whip-100` 窗口内服务端 UDP 丢包 244 个，同配置的正式轮 `in-whip-100` 为 0（见 F6）。

### 7.2 周期分布

`raw/prof2/*`，各列为占服务端 8 核忙碌周期的比例。“Mcycles/viewer·s”按 perf stat 周期计算，频率随负载不同（3000 viewer 的 TCP 输出 1.6～2.0 GHz，GB UDP 2000 为 1.3 GHz，RTMP 6000 和 WHEP 3000 为 2.6～2.7 GHz），不同负载之间不能直接比较。输入类 profile 的 viewers 列是源数（每源 1 个 FLV viewer）。

各列含义：

- send：发送系统调用；
- rx：回环接收 softirq；
- recv：接收调用与 epoll；
- fault：缺页与 mm 系统调用；
- kother：系统调用进出等其他内核开销；
- mux：协议封装；
- crypto：SRTP 与 libcrypto；
- alloc：分配器；
- copy：用户态 memcpy/memset；
- user other：其余用户态，包括 transport、asio、shared_ptr 和转码。

| profile | viewers | Gcycles/s | Mcycles/viewer·s | send | rx | recv | fault | kother | mux | crypto | alloc | copy | user other |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| rtmp-3000 | 3000 | 6.97 | 2.32 | 39.9% | 20.2% | 0.3% | 9.6% | 3.5% | 4.3% | 0.0% | 4.5% | 2.9% | 14.9% |
| rtmp-6000 | 6000 | 16.01 | 2.67 | 40.8% | 21.0% | 0.1% | 6.4% | 2.1% | 6.0% | 0.0% | 3.6% | 3.6% | 16.2% |
| flv-3000 | 3000 | 7.01 | 2.34 | 37.7% | 20.6% | 2.9% | 8.2% | 0.8% | 7.1% | 0.0% | 4.1% | 3.0% | 15.4% |
| rtsp-3000 | 3000 | 7.38 | 2.46 | 35.3% | 18.4% | 0.2% | 8.3% | 2.9% | 9.9% | 0.0% | 4.7% | 3.7% | 16.4% |
| gb_tcp_passive-3000 | 3000 | 7.65 | 2.55 | 33.9% | 16.3% | 0.2% | 13.7% | 3.2% | 4.0% | 0.0% | 4.8% | 10.2% | 13.8% |
| gb_tcp_active-3000 | 3000 | 7.54 | 2.51 | 33.2% | 16.3% | 0.3% | 15.0% | 3.2% | 4.1% | 0.0% | 4.7% | 9.9% | 13.2% |
| gb_udp-2000 | 2000 | 4.70 | 2.35 | 62.7% | 22.1% | 0.2% | 0.0% | 6.4% | 3.6% | 0.0% | 0.1% | 1.1% | 3.8% |
| whep-1000 | 1000 | 5.44 | 5.44 | 40.6% | 15.8% | 0.1% | 0.0% | 5.1% | 5.4% | 17.9% | 3.1% | 2.3% | 9.5% |
| whep-3000 | 3000 | 16.70 | 5.57 | 40.7% | 14.5% | 0.1% | 0.0% | 4.2% | 7.7% | 18.7% | 2.7% | 1.9% | 9.5% |
| rtmp-6000b（重复） | 6000 | 15.91 | 2.65 | 40.6% | 21.0% | 0.1% | 6.5% | 2.2% | 6.1% | 0.0% | 3.5% | 3.7% | 16.4% |
| mix-s1 | 4600 | 11.86 | 2.58 | 41.6% | 19.1% | 0.7% | 2.7% | 2.9% | 7.7% | 3.2% | 4.1% | 3.4% | 14.7% |
| hls-3000 | 3000 | 0.97 | 0.32 | 40.1% | 10.6% | 7.6% | 0.1% | 2.4% | 3.0% | 0.0% | 4.6% | 1.3% | 30.2% |
| in-rtmp-200 | 200 | 1.64 | 8.20 | 11.4% | 19.7% | 26.8% | 0.2% | 2.3% | 15.8% | 0.0% | 3.2% | 4.1% | 16.4% |
| in-rtsp_udp-200 | 200 | 1.31 | 6.53 | 13.6% | 7.7% | 20.3% | 0.4% | 3.6% | 24.4% | 0.1% | 4.0% | 5.7% | 20.2% |
| in-gb_udp-200 | 200 | 1.71 | 8.57 | 11.6% | 7.5% | 17.8% | 0.3% | 3.3% | 32.4% | 0.1% | 3.3% | 5.1% | 18.6% |
| in-whip-100 | 100 | 7.61 | 76.10 | 1.8% | 1.2% | 2.7% | 0.0% | 1.3% | 8.0% | 2.0% | 1.0% | 1.8% | 80.1% |

### 7.3 系统调用量

`syscount.txt`，10 s：

| profile | 发送调用/s | 每 viewer·s | 发送调用累计耗时折合核数 | 其他 |
| --- | --- | --- | --- | --- |
| flv-3000 | sendmsg 225.6 k | 75 | 2.81 | madvise 92/s |
| rtsp-3000 | sendto 221.5 k | 74 | 2.48 | — |
| gb_tcp_*-3000 | sendto 138 k | 46 | 2.11 | — |
| gb_udp-2000 | sendto 507.2 k | 254 | 3.50 | — |
| whep-1000 | sendto 418.9 k | 419 | 2.69 | — |
| whep-3000 | sendto 1224.3 k | 408 | 3.84 | — |
| rtmp-6000b | sendto 442.2 k | 74 | 3.61 | madvise 100/s |
| mix-s1（4600） | sendto 425.1 k + sendmsg 76.6 k | 109 | 3.32 | — |
| hls-3000 | sendmsg 4.5 k | 1.5 | 0.57 | futex 1.1 k/s |

- RTMP、HTTP-FLV、RTSP 每 viewer 每帧一次发送（视频 30 帧/s + AAC 约 46.9 帧/s，合计约 77）。上一轮优化（`25c8607`、`ccd716b`）已把一帧内的 chunk/RTP 包合并为一次写，饱和点以下没有跨帧合并。
- GB TCP 按事件批量写出（`7a258f8`），每 viewer 每秒约 46 次。
- UDP 输出每个 RTP 包一次 `sendto`：GB UDP 每 viewer 每秒约 254 次，WHEP 约 410 次（包更小，另有 SRTP 尾部）。
- 发送调用的累计耗时包含回环上同步执行的接收处理，所以 FLV 3000 的 sendmsg 合计 2.81 核，占服务端约 4.3 核中的 65%。
- `futex` 每秒 0～10 次，`madvise` 每秒约 100 次（jemalloc 归还脏页）。

### 7.4 主要热点与调用路径

**TCP 输出（RTMP/HTTP-FLV/RTSP/GB TCP）**

- 内核发送路径 33%～41%。`rep_movs_alternative`（用户数据拷入 skb）在 RTMP 6000 时占 10.0%；skb 与页分配（`__alloc_skb`、`skb_page_frag_refill`、`alloc_pages`）占 6.3%，其中清零新页的 `clear_page_erms` 占 2.7%。其余是 TCP 发送、IP 输出和回环发送的逐包开销。
- 回环接收 softirq 16%～21%：客户端在同一主机，接收协议处理落在服务端核上（`__inet_lookup_established`、`tcp_rcv_established`、`ep_poll_callback`）。真实网卡部署时这部分移到客户端机器，服务端改为承担网卡驱动的发送与完成中断。这部分属于测试环境产物，不应作为优化目标。
- 缺页 6%～15%：每 viewer 每秒约 30 次。缺页主要发生在 `__memmove_evex_unaligned_erms` 内部，即向新分配的写缓冲拷贝时首次触碰页面。jemalloc 通过 `madvise` 归还脏页，再分配时重新缺页；第 7.6 节的 A/B 证实了这一点：改为先 `MADV_FREE` 后，缺页减少 50%～62%。GB TCP 的会话缓冲 `output_` 按最大帧保留，再拷入 `tcp_transport`，用户态 memcpy 占 10%，包含缺页的拷贝合计 23%。
- 用户态写路径：`tcp_transport::write/post_write/on_write` 合计 5.4%（RTMP 6000），还要加上分配器 3.6%～4.8%、`shared_ptr` 引用计数 1.3%～3.4% 和 asio 调度 2%～3%。每次写都要拷成新 vector、`post` 一次、`make_shared` 一次、入 deque，再 `async_write` 一次（`media/net/tcp_transport.cpp:26-37,49-77`）。
- 协议封装 4%～10%：RTMP 为 `rtmp_chunk_write` 和 `rtmp_server_send_*`，RTSP 为 `rtsp_muxer_input`、`rtp_onsend`，HTTP-FLV 为 beast chunk 写。封装本身不是主要成本。

**UDP 输出**

- GB UDP：内核占 91%（发送 62.7%，回环接收 22.1%，系统调用进出 6.4%），用户态不到 9%。发送路径中，skb 分配 14.2%，未 connect 的 socket 每包查路由（`ip_route_output_flow`）4.4%，IP ID 生成（`__ip_select_ident`）2.5%。
- WHEP：发送 40.7%，SRTP 18.7%，封装 7.7%。libcrypto 无符号，调用链断开，所以 SRTP 按 DSO 自身占比统计。其中 OpenSSL 3 每包重新初始化 cipher 的参数处理（`OSSL_PARAM_locate`、`EVP_CIPHER_up_ref`、`strcmp` 等）约 4.6%。每包查路由 3.3%。
- 每 viewer 周期 WHEP 是 GB UDP 的 2.4 倍，来自 SRTP 和更多的包。

**输入**

- RTMP/RTSP 输入：H.264 AnnexB 起始码扫描（`mpeg4_h264_annexb_nalu`）在 RTMP 200 路时占 7.6%，在 RTSP UDP 200 路时占 13.9%。低负载时 worker 频繁进出 epoll，上下文切换 27～46 k/s，接收和 epoll 占 20%～27%。GB UDP 输入 200 路时 PS 解复用对 H.264 扫描两遍：`mpeg_h264_find_new_access_unit` 找访问单元边界占 11.5%，`mpeg4_h264_annexb_nalu` 占 10.8%，合计 22%，这是 GB 输入每源成本约为 RTMP 两倍的主要原因（第 5.1 节）。两个函数都在 `third/ireader`。该轮带采样时服务端丢包 12 个（1.78 核），见 F6。
- WHIP 输入：FFmpeg 原生 AAC 编码约占 66%。按自身占比：`search_for_quantizers_twoloop` 28.5%，各 `quantize_and_encode_band_cost_*` 合计 18.9%，`cbrtf` 4.0%，心理声学、TNS、M/S 搜索和 libm 约 15%。Opus 解码（`pvq_decode_band` 等）约 9.7%，SRTP 解密 2%，网络约 6%。编码器以 `avcodec_open2(encoder, codec, nullptr)` 打开（`media/codec/audio_transcoder.cpp:206`），使用默认的 twoloop 量化器和全部编码工具。

### 7.5 资源瓶颈判断

- **CPU**：所有协议的第一瓶颈（第 4 节）。
- **内存带宽**：不是第一瓶颈，但在 TCP 输出的容量点附近是重要的次要压力。本机只有 3 个内存通道生效（第 2 节），理论带宽 57.6 GB/s。uncore IMC 计数是整个插槽的，包含同插槽客户端进程的访存，下面的数字是服务端与客户端合计（`perf-stat-imc.txt`，15 s，6 个 IMC 实例合计）。RTMP 6000 时读 13.7 GB/s、写 9.7 GB/s，合计 23.4 GB/s（`rtmp-6000b`；`rtmp-6000` 为 23.6），占理论值的 41%；按 viewer 数线性外推，8500 约 33 GB/s（58%），没有实测。服务端自身的份额更低，但内存控制器由两边共享，服务端实际承受的就是这个合计压力，这一点和回环一样是测试环境的特点。TCP 输出受访存限制：IPC 0.6，LLC 未命中率 0.73，同频下每 viewer 周期随负载上升（第 4 节 `cap3-rtmp-s100`），worker 数增加时每 viewer 指令数不变而 IPC 下降（第 7.7 节），符合共享 L3 和内存带宽竞争、访存延迟随之增大的特征。插满 6 个通道的同型号主机，TCP 输出的容量可能更高，本测试无法验证。UDP 输出的 IMC 流量低一个数量级（GB UDP 2000 为 1.8 GB/s，WHEP 3000 为 6.4 GB/s）。
- **锁与调度**：不是瓶颈。futex 每秒 0～10 次（HLS 1.1 k/s，总耗时 0.01 核）；满载时上下文切换 0.7～1.6 k/s。`offcputime` 在 rtmp-6000b、mix-s1、whep-3000、gb_udp-2000 中记录的 off-CPU 时间 100% 是 `epoll_wait`（17.7～39.5 线程·秒/10 s，正好等于空闲的 worker 时间），没有锁等待、条件变量或阻塞 I/O。8 个 worker 负载均匀。`sink_group` 每帧每组一次互斥锁，没有竞争迹象。
- **异步队列**：饱和点以下不是瓶颈；饱和点附近决定过载行为。drain 在 sink 所属 worker 上执行（`worker_sink_dispatcher.h:109`），`tcp_transport::write()` 把写 `post` 到同一 worker。drain 不返回时（F4），这个 worker 上的写全部推迟，返回后集中执行，导致内存膨胀和集体断开（F5）。这一因果来自代码路径推断，没有逐 viewer 打时间戳证实。
- **网络**：回环无带宽限制，UDP 输出零丢包，TCP 只有 TLP 伪重传（第 6.3 节）；UDP 输入的接收缓冲是例外（F6）。

### 7.6 分配器衰减 A/B

jemalloc 5.3.0 默认参数（用被测 ELF 的 `stats_print` 核对）：`dirty_decay_ms:10000`、`muzzy_decay_ms:0`、`background_thread:false`、`retain:true`。`muzzy_decay_ms:0` 表示脏页衰减到期后直接 `MADV_DONTNEED`，这些页再次分配时重新缺页并清零。参数通过 `MALLOC_CONF` 环境变量传给服务端，不改代码；同一运行包，RTMP/FLV 5000，默认和变体交替运行。

| 运行 | 配置 | 服务端核 | 内核态核 | Mcycles/viewer·s | Minstr/viewer·s | 缺页/s | PSS |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `m1/m2-rtmp-5000-default-*`（4 轮） | 默认 | 5.30～5.35 | 2.56～2.61 | 2.07～2.10 | 1.580～1.586 | 15.3～15.8 万 | 1569～1659 MiB |
| `m2-rtmp-5000-muzzy10s-*`（2 轮） | `muzzy_decay_ms:10000` | 5.21～5.22 | 2.38～2.42 | 1.96～1.98 | 1.464～1.468 | 5.7～6.0 万 | 1816～1833 MiB |
| `m1/m2-flv-5000-default-*`（4 轮） | 默认 | 5.33～5.39 | 2.43～2.55 | 2.13～2.22 | 1.765～1.832 | 14.1～19.0 万 | 1183～1236 MiB |
| `m2-flv-5000-muzzy10s-*`（2 轮） | `muzzy_decay_ms:10000` | 5.29～5.34 | 2.30～2.35 | 2.07～2.10 | 1.690～1.720 | 7.2～9.1 万 | 1384～1392 MiB |
| `m1-*-5000-nodecay-*`（4 轮） | `dirty_decay_ms:-1,muzzy_decay_ms:-1` | — | — | — | — | — | warmup 中超过 16 GiB 预算，BLOCKED |

- **`muzzy_decay_ms:10000`**：脏页到期后先 `MADV_FREE`，页表映射保留；10 s 内再次使用不会缺页，内存紧张时内核仍可回收。缺页减少 50%～62%，每 viewer 指令数 RTMP 降 7.4%、FLV 降 5%（指令数与频率无关，是更可靠的比较口径），内核态少 0.13～0.2 核。代价是 PSS 多 190～240 MiB（`MADV_FREE` 的页在回收前仍计入 RSS/PSS）。
- **关闭衰减**：4 轮的 PSS 都随 viewer 数持续上涨，准备阶段结束时 12.2～13.9 GiB，warmup 中超过 16 GiB 预算，runner 判 BLOCKED（`budget_samples`）；默认参数在同一负载下稳定在约 1.5 GiB。可见关闭衰减后释放的内存没有被有效复用，膨胀速度接近写路径的分配速率（5000 viewer × 2 Mbps 约 1.25 GB/s）。机制没有查证，可能与脏 extent 延迟合并有关。结论：关闭衰减不可行。

容量点验证（同一运行包，与 `cap7` 相同的参数，默认和 muzzy 交替运行）：

| 计数 | 配置 | 结果 | 实时比例最小 | Minstr/viewer·s | PSS 峰值 | 数据 |
| --- | --- | --- | --- | --- | --- | --- |
| RTMP 8750 | 默认 | 0/4（`cap7` 0/3 + `m3` 0/1） | 0.966～0.973 | 1.435～1.478 | 8.1～10.2 GiB | `cap7-rtmp`、`m3-rtmp-8750-default-1` |
| RTMP 8750 | muzzy 10 s | **3/3** | 0.983～0.985 | 1.412～1.436 | 5.2～6.9 GiB | `m3-rtmp-8750-muzzy10s-1..3` |
| RTMP 9000 | muzzy 10 s | 0/2（F5：progressing 6711～6783/9000） | 0.970～0.976 | 1.325～1.332 | 14.4～15.4 GiB | `m3-rtmp-9000-muzzy10s-1..2` |
| HTTP-FLV 8500 | 默认 | 0/4（`cap7` 0/3 + `m3` 0/1） | 0.971～0.975 | 1.606～1.622 | 7.2～8.4 GiB | `cap7-flv`、`m3-flv-8500-default-1` |
| HTTP-FLV 8500 | muzzy 10 s | 2/3 | 0.979～0.986 | 1.576～1.584 | 4.5～6.1 GiB | `m3-flv-8500-muzzy10s-1..3` |
| RTSP TCP 8250 | 默认 | 0/2（`cap2b` 0/1 + `m3` 0/1；`m3` 为 F5：progressing 7219/8250） | 0.946～0.948 | 1.855～1.875 | 13.1～13.2 GiB | `cap2b-rtsp`、`m3-rtsp-8250-default-1` |
| RTSP TCP 8250 | muzzy 10 s | **2/2** | 0.992～0.993 | 1.813～1.816 | 5.1～5.3 GiB | `m3-rtsp-8250-muzzy10s-1..2` |

在 8 核满载、频率相同（约 2.70 GHz）的条件下：

- RTMP 稳定容量从 8500 提高到 8750（+3%）。
- HTTP-FLV 8500 从 0/4 变为 2/3 的边际。
- RTSP 8250 从 0/2 变为 2/2，实时比例 0.992，还有余量（默认配置的稳定容量是 7750）。

满载时每 viewer 指令数只少 2%～3%，比 5000 viewer 时的 5%～7% 小，因为饱和点附近周期主要受访存限制（第 7.5 节）。但饱和点附近是悬崖：默认配置的失败轮次同时出现积压和 PSS 膨胀（F5），每次重新分配都要缺页清零，这加剧了落后；muzzy 让这些页不必重新缺页，同一计数下，默认配置失败轮次的 PSS 是 muzzy 通过轮次的 1.2～2.5 倍。所以几个百分点的指令差异，表现为容量点上通过与失败的差别。

### 7.7 worker 数扩展性

`062-scaling`：每个 worker 分到相同数量的 viewer（RTMP 600、GB TCP passive 1000、GB UDP 700），服务端分别绑定 1、2、4、8 个核（`--workers N --server-cpus 0-(N-1)`），其余参数与其他固定负载轮次相同。12 轮全部 PASS。

| 输出 | worker | viewer | 服务端核 | 频率 | ms/viewer·s | Mcycles/viewer·s | Minstr/viewer·s | IPC | 缺页/viewer·s | 数据 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| RTMP | 1 | 600 | 0.61 | 1.52 GHz | 1.01 | 1.53 | 1.612 | 1.05 | 34.5 | `k1-rtmp-1w-600` |
| RTMP | 2 | 1200 | 1.21 | 1.63 GHz | 1.01 | 1.66 | 1.608 | 0.96 | 35.1 | `k1-rtmp-2w-1200` |
| RTMP | 4 | 2400 | 2.48 | 1.67 GHz | 1.04 | 1.73 | 1.619 | 0.93 | 36.9 | `k1-rtmp-4w-2400` |
| RTMP | 8 | 4800 | 5.20 | 1.88 GHz | 1.08 | 2.04 | 1.595 | 0.78 | 33.2 | `k1-rtmp-8w-4800` |
| GB TCP passive | 1 | 1000 | 0.68 | 2.12 GHz | 0.68 | 1.45 | 1.320 | 0.91 | 45.5 | `k1-gb_tcp_passive-1w-1000` |
| GB TCP passive | 2 | 2000 | 1.31 | 2.36 GHz | 0.65 | 1.54 | 1.329 | 0.86 | 44.4 | `k1-gb_tcp_passive-2w-2000` |
| GB TCP passive | 4 | 4000 | 2.82 | 2.69 GHz | 0.70 | 1.89 | 1.356 | 0.71 | 40.5 | `k1-gb_tcp_passive-4w-4000` |
| GB TCP passive | 8 | 8000 | 6.94 | 2.75 GHz | 0.87 | 2.38 | 1.275 | 0.53 | 39.2 | `k1-gb_tcp_passive-8w-8000` |
| GB UDP | 1 | 700 | 0.68 | 2.17 GHz | 0.97 | 2.10 | 2.675 | 1.27 | 0 | `k1-gb_udp-1w-700` |
| GB UDP | 2 | 1400 | 1.33 | 2.20 GHz | 0.95 | 2.11 | 2.689 | 1.27 | 0 | `k1-gb_udp-2w-1400` |
| GB UDP | 4 | 2800 | 2.65 | 2.48 GHz | 0.94 | 2.35 | 2.694 | 1.14 | 0 | `k1-gb_udp-4w-2800` |
| GB UDP | 8 | 5600 | 5.46 | 2.57 GHz | 0.98 | 2.51 | 2.692 | 1.07 | 0 | `k1-gb_udp-8w-5600` |

- **软件层面线性扩展**：每 viewer 指令数在 1～8 个 worker 间变化不超过 ±3%，每 viewer 的上下文切换（0.08～0.21 次/s，合计每秒不到 900 次）和缺页也不随 worker 数增加。worker 之间没有随核数增长的锁、协调或跨线程开销。
- **硬件层面有共享资源竞争**：每 viewer 的周期和 CPU 时间随 worker 数上升，IPC 下降。RTMP 从 1 到 8 个 worker，CPU 时间 +7%、周期 +34%（频率也从 1.52 GHz 升到 1.88 GHz）；GB TCP 是 +27% 和 +65%，二次拷贝使它访存最重；GB UDP 的 CPU 时间不变，周期 +20%。
- **原因是共享 L3 和内存**：同负载密度下比较 `scale-rtmp-1w-600` 与 `scale-rtmp-8w-4800` 两轮 profile：
  - LLC 未命中率从 29% 升到 69%，每 viewer 每秒 LLC 未命中从 7.8 千次升到 1.8 万次；
  - 插槽 IMC 流量（含客户端）按 viewer 平均从 2.0 MB/s 升到 4.0 MB/s；
  - 周期在各类别间的分布几乎不变（发送 41%，回环接收 21%，缺页 7%～8%），说明是所有代码路径一起变慢，而不是某个组件出现竞争。
  - 1 个 worker 时它独占 22 MiB L3，8 个 worker 时每个只能分到约 1/8。
- **结论**：
  - 8 核容量不能由单核测量线性外推。按 1 worker 的每 viewer 周期（1.53 M）外推，8 核满载 21.6 Gcycles/s 可支撑约 14000 个 RTMP viewer，是实测 8500 的 1.66 倍。
  - 减少每 viewer 的访存量（A1 去掉逐次分配和拷贝、A5 减少缺页清零）比减少指令更直接对应容量。
  - 增加核心数只能部分转化为容量；本轮按约束不做。
