# WHIP 输入性能瓶颈定位与优化（2026-10-27）

## 0. 结论

**WHIP 在约 200～250 路时出现单 worker 饱和，主因是 worker 分布不均。8 核计算上限约在 255～265 路，链路中没有可以不改变音质而明显削减的开销。**

- **单路成本恒定**：每路 WHIP 输入约 141 M 指令/s、约 70 Mcycles/s（IPC 2.0）。
  - 不同 worker、不同负载下，每路指令数相差不到 2%，不存在“同样数量的会话成本不同”。
- **分布不均（主因）**：会话原先跟随建会话的 HTTP 连接。连接轮转与 FLV 观看、就绪探测等其他连接混合，WHIP 会话的落点近似随机。
  - BASE 实测 8 个 worker 的会话数：100 路 9～15；150 路 15～25；200 路 16～31 和 20～35（两次）；250 路 25～36。
  - 分得最多的 worker 先跑满（约 2.2～2.4 GHz 时 31～34 路），接收落后并丢包，其余 worker 只有 0.72～0.87。
- **计算上限**：
  - 平均分布后，新版本 250 路时 8 个 worker 都是 0.93～0.97，丢包为 0。
  - 265 路时 8 个 worker 全部 1.000，8 核共 18.4 Gcycles/s，与“265 × 70 M”相符。
  - 这是固定 8 核下由 Opus→AAC 转码决定的整体上限。
- **成本构成**：AAC 编码 71%，Opus 解码 11%，网络、内核与调度约 7%，内存复制 2.3%，H.264/RTP 解复用 2%，内存分配 1.1%，SRTP 0.9%，重采样 0.3%。
  - 转码以外的部分合计约 12 Mcycles/s，没有低风险的削减点。
- **同步处理时间不是丢包原因**：未饱和时，单次转码最长 1.6～2.1 ms，单个 SRTP 包处理最长 1.6 ms，在 256 KiB 接收缓冲面前可以忽略。只有 worker 持续饱和时，单次耗时才会被拉长到 15～43 ms，并在该 worker 的 socket 上丢包。

| 项目 | 结论 | 提交 | 依据 |
| --- | --- | --- | --- |
| C1：WHIP 会话放到会话最少的 worker | **ACCEPT** | `83bf251` | 225 路：BASE 2/3、新版本 3/3；250 路：BASE 0/3、新版本 3/3。每路成本不变，各 worker 负载差从最多 0.28 降到不超过 0.04 |
| C2：AAC 编码改用 fast 量化器 | **REJECT**（未实施） | — | 本地实测 AAC 编码只省约 23%（单路约 16%），并改变编码质量 |
| C3：关闭 AAC 编码工具（PNS/TNS/IS/MS） | **REJECT**（未实施） | — | 实测反而更慢（+16%），同样改变音质 |
| C4：再加大 UDP 接收缓冲 | **REJECT** | — | 丢包只发生在持续饱和的 worker，缓冲只会加深积压（A3 已有结论，本轮复核） |
| C5：减少接收路径的复制与分配 | **REJECT**（未实施） | — | 复制加分配合计约 3.4%，收益有限，不值得引入额外改动 |

### 容量（固定 8 核，H.264 720p30 + Opus 双声道，每源 1 个 FLV 观看端）

| 类型 | BASE（`28b8ede`） | FINAL（`83bf251`） |
| --- | --- | --- |
| 稳定容量（总负载 ≤ 7.2 核，单 worker < 0.9，可重复通过） | 175 | 225 |
| 最大持续媒体交付（无丢包、实时比 ≥ 0.98，可重复通过） | < 200（200 路 5/6，225 路 2/3，250 路 0/4） | 250（3/3），265 0/2 |
| 8 核计算上限 | — | 约 255～265（265 路时 8 个 worker 全部 1.000） |

---

## 1. 版本、环境与固定 8 核

**版本**
- BASE_HEAD：`28b8ede`（开始时等于 `origin/main`，工作区干净）。被测 ELF 为 `dd099cfc3555877389f50d7af61fdbca412486d8ebb00a2f61e5c4faa58a5d0f`（运行包 `rt-a3f`）。
- FINAL_HEAD：`83bf251`。被测 ELF 为 `e50918bddcb4230c3138ecb29f718129791d5d04e2a621eef5e0790aed83d91a`（运行包 `rt-wb`）。
  - 本地 `build/media_server` 由提交后的工作树构建，工作区干净。
- `rt-wb` 由 `rt-a3f` 复制而来，只替换 `media_server`。客户端、fixture、Python 工具完全相同，例如两版 `whip_publisher` 都是 `322b18ed…`。
- 未 push。

**环境（<TEST_HOST>）**
- CPU：Intel Xeon Silver 4216，16 核 32 线程，1 个 NUMA 节点，调频策略 schedutil，最高 3.2 GHz。
- 绑核：
  - 服务端绑定 CPU 0-7（8 个物理核），8 个 worker 线程；
  - 发布、观看客户端和 runner 绑定 8-15、24-31；
  - 服务端核的 SMT 兄弟 16-23 在所有正式轮中繁忙度为 0.002～0.02；
  - perf 和 bpftrace 绑在 15 号核。
- 网络：独立 netns，回环地址 198.19.0.10/11。
- 客户端未达到瓶颈：250 路时客户端繁忙约 2.0 核（共 16 个逻辑核），最热单核 0.36～0.6；`whip_publisher` 每个 IO 线程 0.33～0.4 核。
- **调频的影响**：低负载时 schedutil 降频，按时间计算的 CPU 会把各 worker 拉平。
  - 例如 WHIP 100 路时 8 个 worker 的 CPU 时间都约 0.72，但它们分到的会话从 9 到 15 路不等。
  - 因此 worker 负载一律用每线程的 cycles 和 instructions（`perf stat --per-thread`）比较，不用 CPU 时间。

**测量方法**
- 正式 A/B 用 `pr2.sh`/`a3.sh`（perf_run，`--perf-stat`）：warmup 15 s，窗口 60 s。
- 两个版本交替运行：第 1、3 轮先 BASE，第 2 轮先 FINAL。
- 诊断轮（`wd.sh`、`prof3.sh`）带 bpftrace 或 perf 采样，只用于定位，不进入容量结论。

## 2. WHIP 输入完整路径与执行位置

| 步骤 | 代码 | 执行位置 |
| --- | --- | --- |
| `POST /publish/whip/<id>` | `http_session` → `handle_whip_request` → `whip::create` | 接受该 TCP 连接的 worker（`service.cpp` 所有监听共用一个轮转计数） |
| 鉴权、解析 offer、生成 answer、DTLS 证书 | `whip.cpp` | 同上，同步执行 |
| UDP socket、DTLS、计时器、`whip_media_receiver`、`media_stream` | `whip_session` 构造与 `startup` | BASE：同一个 HTTP worker。FINAL：现有 WHIP 会话最少的 worker |
| UDP 接收 → STUN/DTLS/SRTP 分流 | `whip_session::run_udp` / `handle_packet` | 会话所属 worker，每包一次 `async_receive_from` 完成 |
| SRTP 解密 | `srtp_transport::unprotect_rtp` | 同上 |
| RTP 解复用、组帧 | `whip_media_receiver::input_rtp` → `rtsp_demuxer`（`third/`）→ `avpkt2bs` | 同上 |
| H.264 发布 | `media_stream::publish` | 同上；跨 worker 的观看端经 `worker_sink_dispatcher` 分发 |
| Opus 解码 → 重采样 → AAC 编码 | `audio_transcoder::transcode`（FFmpeg 9.0.1 静态库，原生 AAC 编码器，twoloop，128 kbps 双声道，截止频率 20 kHz） | 同上，每个 20 ms 的 Opus 包同步执行一次 |
| AAC 发布 | `media_stream::publish` | 同上 |

## 3. 每 worker 的会话分布与负载

数据：`raw/prof/wd-*`（诊断轮，每轮 8 s 的 per-thread perf stat 加 8 s 的 bpftrace），汇总工具 `tools/wdsum.py`。

- 会话归属的判定：bpftrace 在 `udp_recvmsg` 上按（tid，本地端口）计数。每个端口只出现在一个线程下。

| 轮次 | 版本 | 各 worker 会话数 | 最多 worker | 其余 worker | 结果 |
| --- | --- | --- | --- | --- | --- |
| wd-whip100-r1 | BASE | 9～15 | 0.72 核 | 0.71～0.72（时间被调频拉平） | PASS |
| wd-whip150-r1 | BASE | 15～25 | 0.78 核 | 0.72～0.74 | PASS |
| wd-whip200-r1 | BASE | 16～31 | 31 路：0.977 核，2.16 Gcyc/s | 1.12～1.90 Gcyc/s | PASS |
| wd-whip200-r2 | BASE | 20～35 | 35 路：0.998 核，丢包 4664/s | 0.73～0.92 | FAIL |
| wd-whip250-r1 | BASE | 25～36 | 34～36 路的 3 个 worker：0.998 核，2.41 Gcyc/s，丢包 1.5～3.8 k/s | 0.75～0.87 | FAIL |
| wd-wb-whip200-r1 | FINAL | **25 × 8** | 0.799 核，1.78 Gcyc/s | 0.786～0.798 | PASS |
| wd-wb-whip250-r1 | FINAL | **31～32** | 0.99 核（uprobe 开销约 7%） | 0.986～0.992 | FAIL（诊断开销；同负载正式轮 3/3 PASS） |

- **每路指令数恒定**：BASE 200 路时，31 路的 worker 4.36 Ginst/s，16 路的 worker 2.27 Ginst/s，每路都是 141～142 M/s。
- **单 worker 实际能力**：worker 跑满时只有约 2.2～2.4 GHz，按每路 70 M 周期计算，约 31～34 路。
  - 200 路以上时，服务端核繁忙期间的平均频率为 2.22～2.38 GHz（APERF/MPERF），达不到 3.2 GHz 的单核睿频。
- **失败点的会话数**：BASE 失败轮中，丢包全部来自会话数达到 34～36 的 worker，其他 worker 不丢包。这是“局部 worker 饱和”，不是整体 8 核饱和。

## 4. 每路成本拆分（RTP、SRTP、视频、转码）

数据：`raw/prof/whip150-final`（BASE ELF，150 路，`perf record -a -C 0-7 -F 499` 15 s），分类工具 `tools/leafcost.py`。
- 服务端未启用帧指针，调用栈不完整，所以按叶函数分类。

| 环节 | Gcycles（15 s） | 占服务端繁忙周期 | 每路 Mcycles/s |
| --- | --- | --- | --- |
| AAC 编码（twoloop 量化、band cost、心理声学、TNS/LPC、MDCT、libm） | 113.0 | 71.3% | 50.2 |
| Opus 解码（CELT/PVQ、逆 MDCT） | 17.2 | 10.9% | 7.6 |
| 内核网络、系统调用、调度 | 11.3 | 7.1% | 5.0 |
| 其他用户态（asio 调度、锁、shared_ptr 等） | 6.4 | 4.0% | 2.8 |
| 内存复制 / 清零 | 3.6 | 2.3% | 1.6 |
| H.264 / RTP 解复用、组帧 | 3.2 | 2.0% | 1.4 |
| 内存分配释放（jemalloc、av_frame/av_packet） | 1.8 | 1.1% | 0.8 |
| SRTP 解密 | 1.4 | 0.9% | 0.6 |
| 重采样 / 采样格式转换 | 0.5 | 0.3% | 0.2 |
| **合计** | 158.4 | 100% | **70.4** |

主要热点函数：
- `search_for_quantizers_twoloop` 29.2%；
- `quantize_and_encode_band_cost_*` 合计约 20%；
- `pvq_decode_band` 6.7%；
- `cbrtf` 4.2%。

**本地交叉核对**：用 FFmpeg 9.0.1 CLI 单核处理 120 s 测试音频（`-threads 1`，本地 5.4 GHz 桌面 CPU）：

| 处理 | 用户态时间 |
| --- | --- |
| Opus 解码 | 0.16 s |
| 解码 + AAC 编码（128 kbps，截止 20 kHz） | 0.93 s |
| 解码 + AAC 编码，fast 量化器 | 0.73 s |
| 解码 + AAC 编码，关闭 PNS/TNS/IS/MS | 1.08 s |

**结论**：每路成本的 82% 是 Opus→AAC 转码本身，SRTP 与 RTP 接收合计不到 3%。

## 5. 输入丢包的触发条件

- **丢包只出现在两类 worker 上**：会话数达到 34 路以上的 worker（BASE 随机落点），或 265 路以上全部 8 个 worker 都持续 1.000（FINAL）。
- **丢包过程**：worker 一旦持续饱和，接收协程读取速度低于到达速度。
  - 该 worker 下各 socket 的 Recv-Q 积到 270～477 KB，超过 256 KiB `SO_RCVBUF`（内核按 2 倍计账）后开始丢包；
  - 同一进程里未饱和 worker 上的 socket 偶尔会有瞬时积压（快照中最多约 170 KB），但丢包为 0。
- **转码和 SRTP 不阻塞接收**：未饱和时单次转码最长 1.6～2.1 ms，单包 `handle_srtp`（含转码）最长 1.6 ms（FINAL）到 8 ms（BASE 200 路，热点 worker）。
  - 每个 socket 每秒约 440 个包，256 KiB 缓冲可以容纳数百毫秒的到达量。
  - 因此“同步处理时间过长”不是独立原因，单次耗时变长是饱和的结果。
- **发布端 RTCP/STUN 丢包不计入**：命名空间的 `UdpRcvbufErrors` 包含发布端 socket 的丢包（891aec1 报告已说明），服务端丢包以 `server_evidence.udp_drops` 和每 socket 的 `skmem d` 为准。

## 6. 100～280 路的负载趋势（正式轮，无采样）

数据：`raw/runs/wb-whip*`，汇总工具 `tools/wbab.py`。BASE 另外复用上一轮相同 ELF 的 `20261027-a1a3/raw/runs/a3f-whip200-A3-r1..3` 和 `a3f-whip250-A3-r1`。

| 路数 | 版本 | 通过 | 总负载（核） | 最热 / 最冷 worker（核） | 8 核 Gcyc/s | 每路 Minst/s | 服务端丢包 | 滞后 max（ms） |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 175 | BASE | 3/3 | 5.91～6.07 | 0.76～0.82 / 0.72 | 12.4～12.6 | 141.4～141.7 | 0 | 10～11 |
| 175 | FINAL | 3/3 | 5.84～5.85 | 0.73 / 0.72 | 12.5～12.6 | 142.0～142.1 | 0 | 10～15 |
| 200 | BASE | 5/6 | 6.20～6.35 | 0.87～1.000 / 0.72 | 14.2～14.4 | 140.8～141.0 | 1 轮 230667 | 12～180 |
| 200 | FINAL | 3/3 | 6.04～6.13 | 0.75～0.76 / 0.75 | 14.5～14.6 | 141.8～142.0 | 0 | 10～14 |
| 225 | BASE | 2/3 | 6.81～6.88 | 0.96～1.000 / 0.72～0.74 | 15.8～16.0 | 140.7～143.7 | 1 轮 412540 | 88～1969 |
| 225 | FINAL | 3/3 | 6.75～6.77 | 0.86～0.87 / 0.83～0.84 | 16.1～16.2 | 141.0～141.2 | 0 | 13～16 |
| 250 | BASE | 0/4 | 7.28～7.38 | 1.000 / 0.79～0.82 | 17.0～17.3 | 143.9 | 536177～628348；另 2 轮源就绪超时 | 2029 |
| 250 | FINAL | 3/3 | 7.53～7.54 | 0.96～0.97 / 0.93 | 17.5 | 139.9～140.0 | 0 | 48～67 |
| 265 | FINAL | 0/2 | 7.98～7.99 | 1.000 / 0.99 | 18.4 | 140.7～140.8 | 194389～204891 | ~2000 |
| 280 | FINAL | 0/2 | 8.00 | 1.000 / 1.000 | 18.5 | 146.3～146.4 | 148 万～159 万 | ~1700 |

补充说明：
- **BASE 250 路的两轮源就绪超时**（`wb-whip250-a3f-r1`、`-r3`）：240 s 内有源没有凑齐音视频轨道，判为 FAIL/UNCONFIRMED。891aec1 报告中的 250/300 路有同样现象。由于准备阶段没有每 worker 采样，这两轮不单独归因，但没有一轮能通过。
- **BASE 200 路的 5/6**：包含上一轮相同 ELF 的 `a3f-whip200-A3-r1`（FAIL）。诊断轮 `wd-whip200-r2` 也 FAIL，但它带采样，不计入。
- **FINAL 8 核 Gcyc/s 略高（约 +2%）**：负载平均后各核频率更接近，指令数不变。这是频率分布的结果，不是额外开销。

## 7. 尝试与否决的方案

| 编号 | 方案 | 证据 | 结论 |
| --- | --- | --- | --- |
| C1 | WHIP 会话按现有 WHIP 会话数放到最少的 worker | 第 3、6 节 | **ACCEPT** |
| C2 | AAC 编码 `aac_coder=fast` | 本地实测 AAC 编码 −23%，折合单路约 −16%。FFmpeg 的 fast 量化器是质量更低的搜索方式 | **REJECT**：本轮不以降低音质换 CPU；如需采用，应单独做主观或客观音质评估并由需求方决定 |
| C3 | 关闭 PNS/TNS/IS/MS | 本地实测反而 +16% | **REJECT** |
| C4 | 更大的 UDP 接收缓冲 | 丢包只在持续饱和时出现，未饱和 worker 即使有瞬时积压也不丢包 | **REJECT** |
| C5 | 减少接收路径的复制与分配（如 `convert_samples` 每帧 `av_frame_alloc`、payload 复制） | 分配 1.1%，复制 2.3%，上限约 3% | **REJECT**：收益在测量波动范围附近，不值得改动 |
| — | 删除 Opus→AAC 或改成按需转码 | 属于内部媒体格式和转码体系变化，任务约束不允许 | 未评估 |
| — | 新的线程池、跨 worker 迁移会话或按 CPU 反馈动态调度 | 违反现有架构约束；按会话数分布已经使各 worker 差异 ≤ 0.04 | 不做 |

## 8. 实施的最小修改（`83bf251`）

`media/webrtc/whip.cpp`：
- **入口**：新增 `whip::startup(worker_pool&)`，由 `service::run()` 在创建 worker 后调用。
- **会话表**：原有的会话表（id → `weak_ptr`）每项增加 `worker_index`。
- **选择 worker**：`create()` 在鉴权完成后，从会话表统计每个 worker 的现有会话数，选最少的一个，并在这个 worker 上构造 UDP transport、`whip_session` 和 `whip_media_receiver`。
- **不新增**：调度器、锁、计数器或跨 worker 回调。

`media/webrtc/whip_session.cpp`：
- `startup()` 先设置建立超时计时器，再 `spawn` 接收协程。这样会话对象在目标 worker 开始处理之前已经完整构造。
- 之后的所有操作都在所属 worker 上进行；`shutdown()` 仍投递到所属 worker。

`media/http/*`、`service.cpp`：
- 去掉 WHIP HTTP 路径上不再使用的 `worker_context&` 参数。

`docs/architecture.md`：
- 说明 WHIP 发布会话不跟随 HTTP 连接，以及这样做的原因。

**跨线程构造的安全性**：
- HTTP worker 在目标 worker 的 `io_context` 上打开 socket 并构造计时器，asio 允许不同对象在不同线程上发起操作；
- 接收协程启动前，目标 worker 不会接触这些对象；
- 远端在拿到 answer（`create()` 返回之后）以前，无法完成 STUN 鉴权和 DTLS。

**修改规模**：7 个源文件，+48/−19 行，另有 1 行文档。

## 9. 媒体体验验证（A/B 交替，同一夹具）

数据：`raw/runs/wbux-*`，工具 `tools/avsum.py`。音画探测使用 `fixtures-a1/avsync_whip.mkv`（带音画标记）。

| 场景 | 版本 | 结果 | 首帧 | 帧率 | 视频时间戳 | 音频间隙 / 重叠 | 音画偏差（min/p50/max） |
| --- | --- | --- | --- | --- | --- | --- | --- |
| WHIP 1 路 → RTMP/FLV/RTSP/HLS/WHEP/GB | BASE | PASS | 1.87～2.29 s | 30.0 | 无回退、无间隙 | 0 / 0 | −24.8 ms 恒定（HLS −25.3） |
| 同上 | FINAL | PASS | 1.94～2.36 s | 30.0 | 无回退、无间隙 | 0 / 0 | −24.8 ms 恒定（HLS −25.3） |
| WHIP 200 路 → FLV，音画探测 + 解码探测 | BASE | PASS | 1.72 s | 30.0 | 同上 | 0 / 0 | −25.2 恒定；滞后 max 154 ms |
| 同上 | FINAL | PASS | 2.07 s | 30.0 | 同上 | 0 / 0 | −25.2 恒定；滞后 max 40 ms |
| WHIP 225 路 → FLV，音画探测 | BASE | PASS | 3.29 s | 30.0 | 同上 | 0 / 0 | −25.8 恒定；最热 worker 0.955；滞后 max 215 ms |
| 同上 | FINAL | PASS | 1.50 s | 30.0 | 同上 | 0 / 0 | −25.2 恒定；worker 0.81～0.83；滞后 max 79 ms |

- 音画偏差两版一致，是 AAC 编码器的固定延迟，上一轮已记录。没有半速、DTS 异常或漂移，F1 修复保持有效。
- 首帧时间包含等待自然关键帧（GOP 2 s），单次样本的差异不构成结论。
- 解码探测（ffprobe 解码 6 s）：两版每个输出都得到 H.264 720p 帧 179～180 个、AAC LC 48 kHz 双声道帧 279～281 个，退出码 0。

**fanout**：

| 场景 | BASE | FINAL |
| --- | --- | --- |
| WHIP 80 路 → FLV/RTMP/RTSP/WHEP 各 80 | PASS；实时比 1.0；滞后 ≤ 38 ms；解码全部正常；控制 p99 7.4 ms | PASS；同左；控制 p99 7.5 ms |
| WHIP 200 路 → FLV/RTMP/RTSP 各 200 | PASS；最热 worker 0.958；滞后 max 56 ms | PASS；worker 0.82～0.83；滞后 max 128 ms |
| WHIP 150 路 → FLV/RTMP/RTSP/WHEP 各 150 | FAIL：8 核 7.43，丢包 36 万 | FAIL：8 核 7.85，丢包 14 万 |

- WHIP 200 路三协议 fanout 中，FINAL 的滞后跨度更大（128 vs 56 ms），但实时比相同（0.9994～1.0），只有单次样本。这种跨度主要随各 worker 的繁忙度变化，FINAL 所有 worker 都在 0.82，BASE 多数 worker 只有 0.72。暂作单次观察记录，不判定为退化。
- WHIP 150 路加 WHEP 输出时，每个源还需要一路 AAC→Opus 共享转码（`whep_audio_egress`，在该源第一个 WHEP 观看端所在的 worker 上执行），两版都超出 8 核能力。这是场景设计过重，不作为 A/B 证据。

## 10. CPU、内存、丢包、延迟与控制

- **CPU**：每路指令数两版相同（139.9～142.1 M/s）。225 路时 8 核总繁忙 BASE 6.81～6.88，FINAL 6.75～6.77。
- **内存**：FINAL 的 PSS 与 BASE 相差在 ±5% 以内（例如 200 路 418～420 MiB；225 路 470 vs 492 MiB）。
- **FD 和线程**：两版相同（每路一个 UDP socket；16 个线程）。
  - 清理并等待 10 s 后，FD 回到 32、线程 16，两版一致（`wbux-whip100*`、`wbux-whip200av-*`）。
- **UDP 丢包**：FINAL 在 ≤ 250 路的所有正式轮都是 0；BASE 在 200/225/250 路分别有 1/6、1/3、2/4 轮出现 23 万～63 万丢包（另有 2 轮在 250 路源就绪超时）。
- **事件循环与控制响应**：控制探测每 200 ms 一次。
  - 200 路时 p99：BASE 35 ms，FINAL 28 ms；225 路：33 vs 31 ms；200 路 fanout：9.7 vs 8.8 ms。
  - 两版都没有超过 1 s 的请求。
- **会话建立与关闭**：两版首帧 p50 都在 1～2 s（受 GOP 限制）。发布端被 SIGKILL 后，会话都由 30 s 的 ICE 活动超时关闭，两版一致。

## 11. 正确性、生命周期与 sanitizer

**本地脚本**：`san/scripts/whip_lifecycle.sh`，单进程 4 个 worker，运行 BASE、FINAL release 以及 FINAL 的 TSan/ASan/UBSan 版本。覆盖：
- 15 轮 × 8 路 WHIP 建立和正常 DELETE（共 120 路）；
- 两轮会话建立后对发布端 SIGKILL，同时挂一个 FLV 观看端；
- 建立过程中 SIGKILL（16 路，0.25 s）；
- 建立过程中 SIGSTOP（16 路，0.15 s，40 s 后 SIGKILL）；
- 非法 offer；
- 向会话端口发送 192 个随机 UDP 包（首字节覆盖 STUN/DTLS/RTP/RTCP 范围）；
- 带 8 路活动会话时 SIGTERM。

| 运行 | 创建 | 全部关闭 | ICE 超时 | 建立超时 | 观看端在源结束后退出 | 非法 offer | 垃圾包后发布 | SIGTERM | 诊断 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| wl-base-r1（BASE release） | 165 | 是 | 31 | 2 | rc 0，23.0 s | 400 | 4/4 推进 | 0，4 ms | — |
| wl-wb-r1（FINAL release） | 165 | 是 | 31 | 2 | rc 0，23.0 s | 400 | 4/4 推进 | 0，4 ms | — |
| wl-tsan-r1 | 165 | 是 | 31 | 2 | rc 0，23.0 s | 400 | 4/4 推进 | 0，21 ms | 0 |
| wl-asan-r1 | 165 | 是 | 31 | 2 | rc 0，23.0 s | 400 | 4/4 推进 | 0，108 ms | 0（仅 ucontext 提示） |
| wl-ubsan-r1 | 165 | 是 | 31 | 2 | rc 0，23.0 s | 400 | 4/4 推进 | 0，6 ms | 0 |

- 每个会话的关闭日志有 2～3 次，是已有的幂等关闭（DELETE 和接收协程结束各触发一次），BASE 同样如此。
- sanitizer 版本的 perf_run（`san/runs/pf-{tsan,asan}-whip8`）：8 路 WHIP 输入，输出 RTMP/RTSP/FLV/WHEP 各 16、HLS 8，加 FLV 观看端 churn。两版均 PASS，72/72 推进，诊断 0。
- 测试机 `wbux-whip100stop-*`（发布端 SIGKILL）：两版都在 30 s 后由 ICE 超时关闭全部 100 路，观看端停止推进，FD 回到基线。
  - runner 的 source_stop 判定按 RTMP 的立即结束设计，WHIP 要等 30 s 才能检测到发布端消失，所以 runner 判为 FAIL/UNCONFIRMED，两版一致。
- 测试机 `wbux-whip100churn-*`（FLV 观看端 churn 50/轮）：两版 PASS。
- **RED→GREEN**：分布不均没有单元级的稳定复现方法（取决于连接交错）。替代证据是测试机上的 bpftrace 会话归属：
  - BASE 4 个负载点都不均（第 3 节）；
  - FINAL 的 200 路是 25 × 8，250 路是 31～32；
  - 修改后，BASE 在 225/250 路失败的轮次全部变为通过。

## 12. 未验证与限制

- 只测了单一素材（720p30 约 2 Mbps H.264 + Opus 双声道 20 ms）和单一硬件。不同码率、帧率或单声道下的每路成本没有做控制实验。
  - 第 4 节的拆分表明成本几乎全部来自音频转码，与视频码率关系很小，但本轮没有用不同素材实测。
- 分布只按会话数平均，前提是各 WHIP 会话的成本相同（本轮实测成立）。素材差异很大时（如单声道与双声道混合）仍可能不均，但会显著好于随机落点。
- 会话随连接变化时的长期漂移：选择基于当前存活会话。会话退出后，新会话会自动补到空出的 worker，不做已有会话的迁移。
- 没有运行 Go 信令、Qt 客户端和 CTest；本轮没有修改这些部分。
- 浏览器 WHIP 推流没有测试，所有发布都来自原生 `whip_publisher`。

## 13. 后续是否值得投入

- **WHIP 输入：建议停止优化**。FINAL 在 250 路时 8 个 worker 都在 0.93～0.97，已接近 8 核计算上限。剩余成本的 82% 是转码，削减它只能靠降低 AAC 编码质量或改变内部格式（C2）。
- **WHEP 拉流**（`whep_pull_session`）有同样的 Opus→AAC 转码，也同样跟随 `POST /pull/create` 的 HTTP worker。如果需要更高的 WHEP 拉流接入数，可以按本轮方法单独测量，再决定是否沿用同样的放置方式。
- **WHEP 输出的 AAC→Opus 共享转码**放在该源第一个 WHEP 观看端所在的 worker 上。多源加 WHEP 输出时，这个 worker 可能集中多个转码，属于独立课题。
- **AAC 编码参数（C2）**：如果业务能接受音质变化，应先做音质评估。

## 14. 数据与工具

- `raw/prof/wd-*`：分布诊断（`ss` 快照、每线程 `/proc` 计数、per-thread perf stat、bpftrace）。
- `raw/prof/whip150-final`：perf profile 与 perf-script。
- `raw/runs/wb-whip*`：正式 A/B 与边界测试。
- `raw/runs/wbux-*`：体验、fanout、生命周期。
- `raw/runs/d-wd-*`：诊断轮对应的 perf_run 结果。
- 已删除本地复制的凭据、发布端与就绪探测日志；测试机上的原件保留。
- `tools/`：`wdsum.py`、`leafcost.py`、`wbab.py`、`avsum.py`；`tools/test-host/` 为测试机作业与脚本（`wd.sh`、`wd.bt`、作业 200～205）。
- `san/`：本地生命周期脚本、运行记录与 sanitizer 扫描（`san/run_all.log`）。
