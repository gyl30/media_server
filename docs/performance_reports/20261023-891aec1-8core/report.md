# media_server 8 核全协议性能边界与瓶颈分析

## 摘要

被测版本 `891aec1`，生产代码未修改，工具提交 20 个（`370de80`，未推送）。测试机为 Xeon Silver 4216 裸机，媒体服务绑定物理核 0-7，SMT 兄弟线程 16-23 空置，其余核给客户端和采集工具；网络为隔离 netns 回环。

- **边界**：单源 720p30 2 Mbps 下稳定容量为 RTMP 8500、HTTP-FLV 8250、RTSP 7750、GB UDP 7500、WHEP 3250、GB TCP active 13750（延迟拐点 9500～10000）。HLS 测到 14000 时服务端只用 3.2 核，上限在测试客户端。100 源 RTMP 也是 8500，六协议混合为 7000。WHIP 输入稳定 150 路。
- **第一瓶颈**：所有输出协议都是服务端 CPU。TCP 输出 55%～60% 的周期在内核网络栈，其余受访存限制。线程、锁和调度不是瓶颈：1～8 个 worker 时每 viewer 指令数不变，off-CPU 时间全部是 `epoll_wait`。超过饱和点后没有平缓降级：控制面失去响应（F4），TCP viewer 整 worker 断开，PSS 膨胀到 11～15 GiB（F5）。UDP 输入在 CPU 远未饱和时先因接收缓冲丢包（F6）。
- **长稳**：RTMP 7000、WHEP 3000、GB UDP 6400 各 30 min，带 churn，全部 PASS，资源全部回收，没有漂移。
- **值得投入**：
  - A2：drain 分批让出；
  - A3：UDP 输入 `SO_RCVBUF`；
  - A1：`tcp_transport` 持有待发缓冲并在追加时检查高水位；
  - A5：jemalloc `muzzy_decay_ms:10000`，已实测把 RTMP 稳定容量从 8500 提到 8750、RTSP 从 7750 提到 8250，不改代码；
  - A4：WHIP/WHEP 拉流的 AAC 编码参数。
- **不值得**：锁和线程模型、io_uring、`MSG_ZEROCOPY`、协议封装、输入侧 H.264 扫描、HLS，以及关闭 jemalloc 衰减（PSS 无界增长）。
- **功能缺陷**：
  - F1：WHEP 拉流输入的 FLV/RTMP 时间线半速推进；
  - F2：HLS 的 G.711 用了私有 stream type；
  - F3：观测与接口语义；
  - F4/F5：饱和边缘的行为；
  - F6：UDP 输入接收缓冲。
  - 均只记录，未修复。

判定口径：PASS 为全部 viewer 就绪、前进且实时比例 ≥0.98，并通过清理检查；容量为重复轮次全部通过的最大计数。服务端瓶颈、压测端瓶颈、工具问题和环境限制见第 4 节的边界归类表和第 9 节，判定细则见附录 A。

## 1 基线与 Git 状态

- BASE HEAD：`891aec1`（生产代码基线）。
- FINAL HEAD：`370de80`，`main` 分支，工作区干净，本地领先 `origin/main` 20 个提交，未推送。
- 工具提交（仅 `bench/`、`CMakeLists.txt` 的 `gb_fanout` 目标和 `docs/performance.md`）：
  `1338812` → `60a96c4` → `68a22ae` → `0418e40` → `6ef7c32` → `bb16e5c` → `86052ae` → `c1fd56e` → `14eaecd` → `4930da7` → `1c2ff7d` → `49cf28b` → `f3f5bfb` → `c64e7d8` → `862ee0c` → `c775ca5` → `cad6ddc` → `939c249` → `551a61c` → `370de80`，内容与验证见第 9 节。
- `git diff 891aec1..HEAD` 中生产目录（`media/`、`signaling/`、`main.cpp` 等）无改动；`CMakeLists.txt` 仅新增 `gb_fanout` 测试目标。
- 被测 ELF：`build/media_server` SHA256 `ef9aab2623df5ad7…`，自 `891aec1` 起未重编，各运行包 manifest 中一致。

## 2 测试环境与 8 核绑定

原始记录：`raw/env/test-host-env.txt`。

- 主机：Xeon Silver 4216，1 路 16 核 32 线程，单 NUMA 节点；L2 1 MiB/核，L3 22 MiB；内存 93.9 GiB，主板 4 个 DIMM 槽（P1-DIMMA1/B1/D1/E1），A1、D1、E1 各一条 32 GB DDR4-2400 RDIMM 生效，B1 装有同型号内存条但 `dmidecode` 报 `Size: Unknown`，未生效；CPU 的 6 个内存通道只用了 3 个，理论带宽 57.6 GB/s（`dmidecode -t memory`）；Ubuntu 22.04，内核 6.8.0-84。
- SMT：物理核 N 与逻辑 CPU N+16 为兄弟线程（`thread_siblings_list` 已逐一核对）。
- 绑定：媒体服务 `--threads 8`，affinity 0-7；兄弟线程 16-23 不放任何测试进程，窗口内 `sibling_busy` 0.007～0.03 核。发布端、viewer、runner、签名桩、上游实例与采集工具使用 8-15,24-31。
- 调频：schedutil，turbo 开启，单核最高 3.2 GHz；8 核满载时实测约 2.70 GHz（APERF/MPERF）。未修改任何全局配置。
- 其他电源与内存状态（`raw/env/test-host-power.txt`）：intel_pstate passive；uncore 频率范围 1.2～2.4 GHz（出厂值，未改）；RAPL PL1 100 W、PL2 120 W；EPB 6；power-profiles-daemon `balanced`；THP `madvise`，defrag `madvise`。
- 功耗与温度（`raw/env/envlog.tsv`，10-24 05:15～15:05 每 5 s 采样 uncore 频率、封装/DRAM 功耗、服务端核频率和封装温度，共 6986 个样本，其中 12 个因 RAPL 能量计数回绕为负值，已剔除）：在服务端核 ≥2.6 GHz 的 2690 个样本中，uncore 中位 2.4 GHz，封装功耗中位 68.4 W、最高 89.6 W，DRAM 功耗 7.5～17.4 W，封装温度最高 51 °C。整个测试期间没有达到功耗墙或温度墙。
- 网络：已有 netns `msperf-20261013T011210Z-7788320`，服务端 198.19.0.10、客户端 198.19.0.11，均在该 netns 的 lo 上。
- 主机既有 TCP 配置：`tcp_wmem=409600 6553600 2147483647`、`tcp_rmem=409600 8738000 2147483647`、`netdev_max_backlog=2500000`、`somaxconn=4096`，均为测试前状态，未修改。

## 3 全量协议矩阵

标注与任务要求的判定对应：**通过**（PASS）= 原生客户端 ready/progressing 全部满足、实时比例 ≥0.98，RTMP/RTSP/FLV/HLS 还经 ffprobe 6 s 解码确认音视频帧数；**失败**（FAIL）= 服务端功能问题（附缺陷编号）；**不支持**（UNSUPPORTED）= 服务端按设计拒绝；**工具缺失**（BLOCKED）= 测试工具无法产生该输入；**部分复现**（INCONCLUSIVE）= 重复轮次结果不一致；**未测** = 本轮没有运行，不计入 PASS。小负载矩阵每个输出 20 个 viewer（HLS 5 个），窗口 20～37 s，不是容量数据。

### 3.1 输入 × 输出（H.264 Constrained Baseline 720p30 + AAC LC 48 kHz）

数据：`raw/runs/v4-in-*`、`v5-in-whep_pull`、`d1-*`。WHIP 与 WHEP 拉流的上游媒体是 H.264 + Opus，服务端转成 AAC 后输出。

| 输入 \ 输出 | RTMP | RTSP TCP | HTTP-FLV | HLS | WHEP | GB UDP | GB TCP active | GB TCP passive |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| RTMP 发布 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 |
| RTSP TCP 发布 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 |
| RTSP UDP 发布 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 |
| WHIP | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 |
| RTMP 拉流 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 |
| RTSP 拉流 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 |
| WHEP 拉流 | **失败**（F1，时间线半速） | 通过 | **失败**（F1） | 通过¹ | 通过 | 通过¹ | 通过¹ | 通过¹ |
| GB UDP | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 |
| GB TCP active | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 |
| GB TCP passive | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 |

¹ 原生客户端按 RTP 时间戳（来自 pts）测得实时比例 1.002，HLS 可解码；但 TS/PS 中写入的 dts 同样受 F1 影响，PTS−DTS 持续增大，长时间播放的兼容性没有验证。

### 3.2 编码组合

数据：`raw/runs/v4-codec-*`、`raw/runs/b2-codec-*`。每格 20 个 viewer（HLS 5 个），RTMP/RTSP/FLV/HLS 用 ffprobe 解码 6 s，确认音视频帧数。夹具均为 720p30、GOP 2 s：H.265 Main，Opus 48 kHz 立体声，G.711 8 kHz 单声道。

标注：

- **通过**：全部 viewer 就绪且前进，实时比例 ≥0.98，解码出音视频。
- **F2**（FAIL）：HLS 视频可解码，但 G.711 音频被 ffprobe 识别为 `mp3` 或无法识别，解出 0 帧（缺陷 F2）。
- **仅视频**：HLS 按设计跳过 Opus 轨（`docs/protocols.md`），视频可解码。
- **不支持**：服务端按设计拒绝。
- **工具缺失**：测试工具无法产生该输入。

| 编码 | 输入 | RTMP | RTSP | HTTP-FLV | HLS | WHEP | GB UDP | GB TCP active | GB TCP passive |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| H.265 + AAC | RTMP 发布、RTSP TCP 发布、RTSP UDP 发布、RTMP 拉流、RTSP 拉流、GB TCP active、GB TCP passive | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 |
| H.265 + AAC | GB UDP | 通过² | 通过² | 通过² | **2/4 轮失败（F6）**² | 通过² | 通过² | 通过² | 通过² |
| H.265 + AAC | WHIP | 工具缺失³ | | | | | | | |
| H.265 + AAC | WHEP 拉流 | 不支持⁴ | | | | | | | |
| H.264 + Opus | RTMP 发布¹、RTSP TCP 发布、RTSP UDP 发布、RTMP 拉流¹、RTSP 拉流¹ | 通过 | 通过 | 通过 | 仅视频 | 通过 | 不支持⁵ | 不支持⁵ | 不支持⁵ |
| H.264 + Opus | WHIP、WHEP 拉流 | 见 3.1（Opus 先转 AAC，全部输出可用） | | | | | | | |
| H.264 + Opus | GB 三种输入 | 不支持⁶ | | | | | | | |
| H.265 + Opus | RTMP 发布¹、RTSP TCP 发布 | 通过 | 通过 | 通过 | 仅视频 | 通过 | 不支持⁵ | 不支持⁵ | 不支持⁵ |
| H.264 + G.711A | RTMP 发布、RTSP UDP 发布、RTMP 拉流、RTSP 拉流、GB UDP、GB TCP active、GB TCP passive | 通过 | 通过 | 通过 | F2 | 通过 | 通过 | 通过 | 通过 |
| H.264 + G.711A | WHEP 拉流 | 不支持⁴ | | | | | | | |
| H.264 + G.711U | RTMP 发布、RTSP TCP 发布 | 通过 | 通过 | 通过 | F2 | 通过 | 通过 | 通过 | 通过 |
| H.265 + G.711A | RTMP 发布、RTSP TCP 发布 | 通过 | 通过 | 通过 | F2 | 通过 | 通过 | 通过 | 通过 |

注：

1. **RTMP Opus 发布**：ffmpeg n7.1.1 的 FLV 封装拒绝 Opus（`v4-codec-h264_opus-rtmp` 原判 BLOCKED）。RTMP/RTSP 拉流的上游实例也由 RTMP 推流供给，同样需要 n9.0；`b2-codec-h264_opus-rtsp_pull` 因任务脚本漏配 n9.0 而上游无轨道，已作废，以 `-n9` 重跑为准。经用户同意改用 ffmpeg n9.0.2，以 Enhanced RTMP FourCC `Opus` 发布并解码（第 9 节）。服务端 RTMP/HTTP-FLV 输出 Opus 也用 FourCC，需要 n9.0 的 ffprobe 才能解码。
2. **H.265 + AAC 经 GB UDP**：
   - `b2-codec-h265_aac-gb_udp` 和 `-r3`：HLS 首个分片 10 s 内未生成，runner 中止；两轮整轮 `UdpRcvbufErrors` 分别为 110、108。
   - `-r2`：全部 8 种输出通过；`-hlsonly`：只开 HLS，通过；这两轮丢包为 0。
   - 判为 F6（UDP 输入默认接收缓冲容不下 H.265 关键帧突发），不是 H.265 或 HLS 本身的问题；表中其他输出按 `-r2` 记为通过。
3. **WHIP H.265**：`bench/whip_publisher.cpp:236-237` 只接受 H.264 + Opus 夹具，服务端 WHIP 接收 H.265 的分支本轮未验证。
4. **WHEP 拉流**：offer 只含 H.264 与 Opus（`docs/protocols.md`）。H.265 源时上游返回 400（`whep pull offer rejected ... status 400`）；G.711 源时 answer 中 `audio false`，只拉到视频，源就绪探测要求双轨，因此失败。
5. **GB 输出**：PS sender 只支持 AAC/G.711 音频，Opus 源创建 sender 返回 500（F3），本轮这几格未运行 GB 输出；`v4-codec-h264_opus-rtsp` 中 GB 三组的 create 均失败，可作佐证。
6. **GB 输入**：GB PS receiver 只支持 AAC、G.711 音频（`docs/protocols.md`），工具也没有构造 Opus PS 流，按设计记为不支持。

## 4 各协议容量与失败边界

单源 H.264 720p30 2 Mbps + AAC，RTMP 发布，单一输出协议。“稳定容量”是所有重复轮次都通过的最大计数；“边际”是部分轮次通过的计数。统计用 `tools/capsum.py`，剔除 `analysis/invalid-rounds.txt` 中的作废轮次。容量点的指标取该计数的全部通过轮次。

| 输出 | 稳定容量 | 边际 | 失败点 | 容量点服务端核 | 单核最高 | Gcycles/s | PSS | 实时比例最小 | lag span p50 | 数据 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| RTMP | **8500**（4/4） | — | 8750（0/3）、9000（0/1） | 7.92～7.99 | 1.00 | 21.4～21.6 | 4.0～7.5 GiB | 0.981～0.996 | 453～876 ms | `cap7-rtmp` |
| HTTP-FLV | **8250**（4/4） | — | 8500（0/3） | 7.94～7.97 | 1.00 | 21.5 | 3.6～4.3 GiB | 0.988～0.991 | 537～683 ms | `cap7-flv`、`cap6-flv-confirm` |
| RTSP TCP | **7750**（2/2；7500 为 4/4） | 8000（3/4） | 8250（0/1）、8500（0/1） | 7.85～7.87 | 0.99～1.00 | 21.2～21.3 | 3.2～3.3 GiB | 0.998 | 240～257 ms | `cap2b-rtsp`、`cap6-rtsp-confirm`、`cap6b-rtsp-confirm` |
| GB UDP | **7500**（4/4） | — | 8000（0/2，另有 `diag3`） | 7.00～7.05 | 0.89～0.90 | 19.0～19.1 | 59 MiB | 0.999 | 285～294 ms | `cap2-gb_udp-hung8000`、`cap6-gb_udp-confirm` |
| WHEP | **3250**（4/4） | 3500（3/4） | 3750（0/6） | 6.76～6.88 | 0.85～0.87 | 18.1～18.4 | 608 MiB | 0.999 | 312～410 ms | `cap2-whep`、`cap6*-whep-confirm` |
| GB TCP active | **13750**（4/4，另 `l1` 1/1） | 14000（1/2） | — | 7.88～7.91 | 1.00 | 21.6 | 6.4～6.5 GiB | 0.995～0.996 | 527 ms（`l1`） | `cap2-gb_tcp_active`、`cap2c-gb_tcp_active` |
| HLS | **≥14000**（1/1，搜索上限） | — | 未达到 | 3.19（进程） | 0.45 | 4.35 | 107 MiB | — | — | `cap3-hls` |
| RTMP，100 源 | **8500**（4/4） | — | 8750（0/3）、9000（0/1） | 6.99～7.50 | 0.88～0.95 | 18.9～20.2 | 2.1～2.6 GiB | 0.992～0.996 | 168～251 ms | `cap3-rtmp-s100` |
| 混合六协议等分¹ | **7000**（1/1） | 7500（2/3） | 7750（0/1）、8000（0/1） | 7.47 | — | 20.2 | 1.8 GiB | 0.997 | 322 ms | `cap3-mix` |
| GB TCP passive | **≥14000**（1/1，工具端口上限） | — | 未达到 | 7.91 | 1.00 | 21.6 | 6.7 GiB | 0.990 | 526 ms | `cap2-gb_tcp_passive`、`cap2d-gb_tcp_passive` |

表中为 jemalloc 默认参数。部署参数 `MALLOC_CONF=muzzy_decay_ms:10000` 下，RTMP 8750 为 3/3、RTSP 8250 为 2/2、HTTP-FLV 8500 为 2/3，RTMP 9000 仍然失败（0/2），见第 7.6 节。

失败方式：

- **RTMP / HTTP-FLV / RTSP**：失败点上 8 个服务端核全部 100%，媒体开始落后实时（RTMP 8750 为 0.966～0.973，FLV 8500 为 0.971～0.975，RTSP 8250 为 0.946）。再往上就是 F5：RTMP 9000 时 2 个 worker 上的 2217 个 viewer 同时被服务端断开，PSS 涨到 13.3 GiB；RTSP 8250/8500 时 PSS 11～13 GiB 并出现服务端 EOF。健康点与失败点之间只差 250 个 viewer（约 3%），饱和后没有平缓降级区间。
- **GB UDP**：8000 时媒体面仍然实时（0.997），服务端合计 7.46 核（93%）。失败的是控制面：窗口结束时各组删除 sender 的 HTTP 请求 10 s 超时，只删掉 161～207/8000 个。根因见 F4，同 worker 的媒体分发长时间不让出。所以 GB UDP 的容量受控制面约束，而不是媒体发送。
- **WHEP**：3500 时有 1/4 轮实时比例 0.987（阈值 0.98 以上，但被判饱和）。3750 时爬坡期间服务端核持续 100%，新会话的 ICE/DTLS 处理排不上，出现 `STUN nomination timeout` 和就绪超时，6 轮全部失败。WHEP 先失败在新会话建立，而不是已建立会话的转发。
- **GB TCP active/passive**：约 9500～10000 起服务端核持续 ≥98%，之后每 viewer 周期从 2.6 降到 1.55 Mcycles。原因是 sender 在积压期间把多帧合并成一次写，媒体不丢、不落后（实时比例 ≥0.99），只是交付延迟增大：lag span p50 在 5000、8000、10000、12000、13750 时分别为 143、217、288、407、527 ms。active 14000 有 1 轮失败，原因是爬坡时最后一组 `/gb28181/sender/create` 超过客户端 10 s 时限（F4）。passive 测到 14000 仍通过，但 GB TCP 监听端口从 18700 起、必须低于临时端口下限 32768，14000 已接近工具能分配的上限，不是服务端失败点。按“延迟不随负载增长”的标准，GB TCP 的有效容量在 CPU 拐点 9500～10000 附近。
- **HLS**：4000～14000 全部通过。14000 时服务端 3.2 核（频率 1.25 GHz，4.35 Gcycles/s，约为满载能力的 20%），每 viewer 0.22 Minstr/s；Python HLS 客户端已用 14.1 个逻辑核（共 16 个）。HLS 的上限由测试客户端决定，服务端远未饱和。按指令数外推，服务端 CPU 可支撑数万 HLS viewer，但这一点没有实测。
- **混合六协议**：¹ RTMP、HTTP-FLV、RTSP、WHEP、GB UDP、GB TCP passive 各占 1/6，单源。7500 时 3 轮中 1 轮失败：RTMP/FLV/RTSP 各组实时比例降到 0.964，WHEP 和 GB 组仍然实时。7750 时 RTSP 每组 63～64 个 viewer 在窗口内被服务端断开；8000 时 RTMP、RTSP 各组在测量前就出现服务端 EOF（F5）。混合负载下，TCP 输出最先落后、最先被断开。
  - 按单协议满载周期成本加权，预测容量约 6950（平均 3.11 Mcycles/viewer·s）。实测 7500 时为 2.82 Mcycles/viewer·s（IPC 0.94，单 TCP 协议只有 0.6），比预测高约 9%。访存受限的 TCP 输出和计算受限的 WHEP/GB UDP 在同一核上交错执行，提高了周期利用率。指令数比按单协议累加少 5.5%，与 `mix-s1` 一致。
- **RTMP 100 源**：8500 四轮通过，8750 三轮、9000 一轮全部失败，失败点与单源相同。8500 时服务端平均 7.0～7.5 核，采样区间峰值已到 8.0，频率 2.70 GHz，周期 18.9～20.2 G（满载能力的 87%～94%）。失败方式仍是 F5：每 256 个 viewer 的组有 77～113 个（30%～44%）被服务端 EOF，PSS 涨到 12.3～14.3 GiB。多源时每 viewer 指令数稳定在 1.43～1.45 M（单源 1.54～1.60），但周期数随负载持续上升：同样 2.70 GHz，8500 四轮为 2.22～2.38 Mcycles，IPC 从 5000 时的 1.0 降到 0.61，接近单源的 2.52～2.59。多源在中低负载省下的周期，在饱和点附近大部分被访存竞争抵消，所以容量没有提高。多源的收益在交付延迟：同为 8500，lag span p50 为 168～251 ms，单源为 453～876 ms。

边界归类：

| 类别 | 项目 | 依据 |
| --- | --- | --- |
| 服务端瓶颈 | RTMP、HTTP-FLV、RTSP、GB UDP、WHEP、GB TCP active 14000 的失败，RTMP 100 源，六协议混合；WHIP 输入 200 路以上，GB UDP 输入 300 路 | 失败时服务端核饱和或服务端 socket 丢包；客户端核有余量（每轮 `client_busy` 与自有进程 CPU 记录在 result.json） |
| 压测端瓶颈 | HLS ≥14000 | Python HLS 客户端用了 14.1/16 个逻辑核，服务端 3.2 核 |
| 工具限制 | GB TCP passive ≥14000（监听端口须低于临时端口下限 32768）；WHIP H.265 无发布工具；ffmpeg 7.1.1 无法以 FLV 发布 Opus（已换 n9.0.2 补测） | 第 9 节 |
| 环境限制 | 回环 softirq 计在服务端核上；内存 3 通道且客户端共享插槽带宽；10-24 00:00～01:40 时段漂移 | 第 2、6、7 节 |

所有协议的第一个瓶颈都是服务端 8 个核的 CPU。PSS 最多 7.5 GiB（主机 93 GiB），FD 最多 15039（UDP 每 sender 两个 socket），都不是约束。网络见第 6 节。

时段漂移：10-24 00:00～01:40 的 RTMP/FLV 周期成本比 04:20 以后高约 25%（同一 ELF、同一客户端，指令数相同，IPC 不同），对应 RTMP 容量 6500～6750（`cap2-rtmp`）。表中 RTMP 和 FLV 取 04:20 以后的重测；RTSP、GB、WHEP 在两个时段没有可见差异。详见第 6 节。

## 5 输入、输出与源数量的差异

### 5.1 单路输入开销

`outputs none` 时 runner 仍给每个源保留 1 个 FLV viewer（约 1.8 Minstr/s），下表数值包含它。100 源时服务端只有 1 核左右、频率约 0.8 GHz，空闲进出开销不可忽略。RTMP/RTSP 用 300 源减 100 源得到边际成本：RTMP 5.87 Minstr/源·s，RTSP 6.66。外推截距为空闲基线，约 0.07 Ginstr/s、0.35 Gcycles/s。其余协议用 100 源总量减去该基线再除以源数。

| 输入 | Minstr/源·s（含 1 FLV viewer） | Mcycles/源·s（约 0.8 GHz） | PSS/源 | 结果 | 数据 |
| --- | --- | --- | --- | --- | --- |
| RTMP 发布 | 5.85～5.87 | 4.4～4.6 | 0.7～0.9 MiB | 100/300 PASS | `in-rtmp-100/300` |
| RTSP TCP 发布 | 6.66～6.70 | 4.6～4.7 | 0.8～1.0 MiB | 100/300 PASS | `in-rtsp_tcp-100/300` |
| RTSP UDP 发布 | 6.61 | 4.7 | 1.3 MiB | 100 PASS | `in-rtsp_udp-100` |
| RTMP 拉流 | 5.82 | 6.5 | 0.8 MiB | 100 PASS | `in-rtmp_pull-100` |
| RTSP 拉流 | 7.72 | 7.5 | 1.2 MiB | 100 PASS | `in-rtsp_pull-100` |
| GB UDP | 9.2～10.7 | 5.9～9.0 | 1.0～1.2 MiB | 100 PASS；300 FAIL（UDP 丢包，F6） | `in-gb_udp-100/300` |
| GB TCP active/passive | 9.55 | 8.3 | 1.2 MiB | 100 PASS | `in-gb_tcp_*-100` |
| WHIP（H.264 + Opus） | 134～141 | 62～69（1.2～2.1 GHz） | 2.1～2.2 MiB | 100/150 PASS；200 FAIL（UDP 丢包 98783，F6）；250/300 FAIL（源就绪超时） | `in-whip-100/150/200/250/300` |
| WHEP 拉流（H.264 + Opus） | 221 | 109（1.89 GHz） | 2.4 MiB | 100 FAIL（UDP 丢包 813；F1 使 FLV 半速） | `in-whep_pull-100` |

- 普通输入（RTMP/RTSP/拉流）扣除 FLV viewer 后每源 4～6 Minstr/s，约等于 3～4 个 RTMP viewer。8 核即使接入数百路，输入也只占很小一部分。
- GB 输入扣除 FLV viewer 后每源 7～9 Minstr/s，约为 RTMP 的 2 倍，来自 PS 解复用和 RTP 重组。
- WHIP 和 WHEP 拉流的音频是 Opus，进入内部流前要转成 AAC（`media/codec/audio_transcoder.cpp`，FFmpeg Opus 解码 + 原生 AAC 编码）。每源 140～220 Minstr/s，是 RTMP 输入的 24～38 倍。WHIP 输入稳定容量为 150 路。200 路时服务端只用了满载周期能力的 59%，却因接收丢包失败（F6）；250、300 路在源就绪阶段就超时。限制 WHIP 接入数的是 worker 上同步转码造成的接收延迟，而不是总 CPU。
- RTMP 拉流与 RTMP 发布的指令数相同，周期多 46%（IPC 0.61 对 0.78）。上游实例跑在客户端核上，这部分周期差异来自服务端核上的接收路径，原因未单独分析。

### 5.2 单路输出单 viewer 开销

见第 6.1 节表。按满载周期由低到高：GB TCP（饱和合并写后 1.55）< RTMP 2.52～2.59 ≈ GB UDP 2.53～2.55 ≈ HTTP-FLV 2.57～2.67 < RTSP 2.66～2.80 < WHEP 5.5～5.7 Mcycles/viewer·s。HLS 只有 0.36 Mcycles/viewer·s（0.8 GHz），viewer 按 2 s 分片拉取，服务端共享分片，成本主要是 HTTP 请求处理。

### 5.3 单源与多源

固定 viewer 总数，改变源数（每个源由独立的 RTMP 发布端推流，viewer 均匀分到各源）：

| 负载 | 源数 | Minstr/viewer·s（扣除输入） | PSS | 缺页/s | lag span p50 | 数据 |
| --- | --- | --- | --- | --- | --- | --- |
| RTMP 3000 | 1 | 1.53 | 951 MiB | 84 300 | 131 ms | `ms-rtmp3000-s1` |
| RTMP 3000 | 10 | 1.38 | 659 MiB | 22 100 | 66 ms | `ms-rtmp3000-s10` |
| RTMP 3000 | 100 | 1.31 | 577 MiB | 3 700 | 49 ms | `ms-rtmp3000-s100` |
| RTMP 3000 | 300 | 1.30 | 754 MiB | 940 | 48 ms | `ms-rtmp3000-s300` |
| WHEP 1000 | 1 | 7.45 | 206 MiB | 26 | 204 ms | `ms-whep1000-s1` |
| WHEP 1000 | 10 | 8.15 | 226 MiB | 187 | 97 ms | `ms-whep1000-s10` |
| WHEP 1000 | 100 | 15.0 | 365 MiB | 372 | 61 ms | `ms-whep1000-s100` |

- TCP 输出：单源是内存最差的情况。所有 viewer 在同一时刻为同一个关键帧各分配一份整帧拷贝（3000 × 88 KB ≈ 260 MB 的瞬时分配），分配器只能取新页，稍后再归还，于是每秒 8.4 万次缺页、PSS 偏高。源一多，各源关键帧在时间上错开，释放的内存马上被复用，缺页降到 1/20～1/90，每 viewer 指令少 15%，交付延迟抖动从 131 ms 降到约 50 ms。但容量没有因此提高：`cap3-rtmp-s100` 的稳定容量也是 8500。负载越高，每 viewer 周期越接近单源（8500 时 2.22～2.38 对 2.52～2.59 Mcycles），饱和点附近同样受访存限制（第 4 节）。多源的实际收益是同等负载下更低的 CPU、内存和交付延迟抖动，而不是更高的上限。
- WHEP：源越多越贵。AAC→Opus 按源共享（D03），100 源就有 100 个编码器；(15.0 − 7.45) × 1000 / 100 ≈ 75 Minstr/s 的固定成本落在每个有 WHEP viewer 的源上。单源时它被 1000 个 viewer 摊薄，看不出来。WHEP 多源场景的容量应按“每源 75 Minstr/s + 每 viewer 7.4 Minstr/s”估算。

### 5.4 输出组合

`mix-s1`：单源同时带 RTMP 1000、HTTP-FLV 1000、RTSP 1000、GB UDP 500、GB TCP passive 500、WHEP 300、HLS 300，共 4600 个 viewer。`mix-s50` 把同样的 viewer 分到 50 个源。两轮全部 PASS，所有 viewer 实时比例 ≥0.999。

| 轮次 | 实测 Ginstr/s | 按单协议成本累加的预测 | 差 | Gcycles/s（频率） | PSS | 缺页/s | lag span p50 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `mix-s1` | 9.09 | 9.69 | −6% | 9.23（1.74 GHz） | 990 MiB | 37 400 | 172 ms |
| `mix-s50` | 12.74 | 13.57 | −6% | 11.54（1.93 GHz） | 878 MiB | 1 960 | 55 ms |

预测 = Σ（viewer 数 × 第 6.1 节的每 viewer 指令数）+ Σ（源数 × 输入成本）+ 有 WHEP viewer 的源数 × 75 Minstr/s（AAC→Opus）+ 空闲基线。两轮实测都比预测低 6%，说明各协议输出之间没有额外的相互干扰，同一源上的多种输出只共享输入和派生封装。容量可以按“各协议每 viewer 成本 + 每源固定成本”线性估算，再留出约 6% 的余量。`mix-s50` 比 `mix-s1` 多出的 3.7 Ginstr/s 主要来自 50 个 AAC→Opus 编码器。

容量上，六协议等分的单源混合负载稳定在 7000，7500 为边际（第 4 节）。按单协议周期成本加权平均估算会低估约 9%，因为混合后 IPC 更高。估算混合容量时，用指令数线性相加，再按混合 IPC 约 0.94、满载 21.6 Gcycles/s 换算，比用单协议周期数相加更准确。

## 6 CPU、PSS 与网络指标

### 6.1 每 viewer 成本

成本用服务端 8 核的 APERF 周期和 perf stat 指令数，除以 viewer 数和窗口秒数。频率随负载变化：低负载 0.8～1.2 GHz，满载约 2.70 GHz。

| 输出 | Minstr/viewer·s（与频率无关） | Mcycles/viewer·s，满载 2.7 GHz | IPC，满载 | IPC，约 1 GHz | PSS/viewer | 缺页/viewer·s | 数据 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| RTMP | 1.54～1.60 | 2.52～2.59 | 0.58～0.62 | 0.97～1.05 | 361 KiB | 17～32 | `fx-rtmp-*`、`cap7-rtmp` |
| HTTP-FLV | 1.66～1.81 | 2.57～2.67 | 0.64～0.70 | 0.95 | 303 KiB | 26～36 | `fx-flv-3000`、`cap7-flv`、`cap6-flv-confirm` |
| RTSP TCP | 1.96～2.06 | 2.66～2.80 | 0.73～0.75 | 1.01 | 380 KiB | 34～47 | `fx-rtsp-3000`、`cap*-rtsp*` |
| GB TCP（未合并写） | 1.23～1.33 | 2.5～2.7 | 0.48～0.50 | 0.93 | 386～424 KiB | 31～34 | `fx-gb_tcp_*`、`cap2-gb_tcp_*` 6000～8000 |
| GB TCP（饱和合并写） | 0.79 | 1.55 | 0.51 | — | ~490 KiB | — | `cap2c/cap2d` 13750～14000 |
| GB UDP | 2.69～2.70 | 2.53～2.55 | 1.06 | 1.26 | 5 KiB | ≈0 | `fx-gb_udp-2000`、`cap*-gb_udp*` |
| WHEP | 7.29～7.45 | 5.5～5.7 | 1.29～1.31 | 1.45 | 183 KiB | ≈0 | `fx-whep-*`、`cap*-whep*` |
| HLS | 0.24 | — | — | 0.63（813 MHz） | ≈0 | ≈0 | `fx-hls-3000`，容量见第 4 节 |

要点：

- 每 viewer 指令数与负载基本无关（RTMP 1000/3000/5000/8000 为 1.38/1.54/1.56/1.60 M，1000 时含较多空闲开销），周期数则随频率上升。RTMP 从 800 MHz 到 2.7 GHz，IPC 由 1.05 降到 0.62，每 viewer 周期从 1.27 M 增至 2.58 M，换算成时间只从 1.59 ms/s 降到 0.96 ms/s。这是访存停顿的特征：停顿以纳秒计，频率越高，每次停顿折合的周期越多。所以低负载的 CPU 时间不能线性外推到满载。
- TCP 输出（RTMP/FLV/RTSP/GB TCP）IPC 0.5～0.75，访存受限；每 viewer 300～420 KiB 用户态内存，每秒约 30 次缺页。UDP 输出（GB UDP/WHEP）IPC 1.06～1.3，几乎不缺页。差别来自每帧每 viewer 的整帧拷贝（见第 7 节）。
- WHEP 每 viewer 指令数是 RTMP 的 4.7 倍，周期是 2.2 倍。它是计算受限（SRTP 加密和逐包处理），不随时段漂移。
- 用 `/proc/stat` 统计的繁忙核数与服务端进程 CPU 时间在负载 ≥1 核时相差不到 2%。负载极低时（`in-rtmp-100`）`/proc/stat` 只计到 0.40 核，进程实际为 0.79 核（tick 采样偏差），C0 驻留折合 0.97 核（含空闲进出开销）。输入成本因此用 300 源减 100 源的差值计算（第 5 节）。

### 6.2 内存

- 容量点 PSS：GB UDP 59 MiB，WHEP 608 MiB，RTSP 3.3 GiB，FLV 3.6～4.3 GiB，RTMP 4.0～7.5 GiB，GB TCP 6.4～6.9 GiB。主机 93 GiB，可用 83 GiB 以上，内存不是瓶颈。
- 超过饱和点后，TCP 输出的 PSS 在一个窗口内涨到 8～13 GiB（RTMP 9000、RTSP 8250/8500、FLV 8500），机制见 F5。
- 服务端 TCP 套接字的内核发送队列合计 ≤2 MiB（每 5 s 采样），接收队列 0。回环上客户端及时读取，内核缓冲不是内存去向；PSS 主要是用户态写队列和分配器保留的页。分配器的影响见第 7 节和 jemalloc A/B 结果。
- 不缺页、PSS 几乎恒定的 GB UDP 说明，按包发送、包大小落在小对象 size class 的路径对分配器友好。

### 6.3 网络

- 每 viewer 稳态媒体 2.13～2.17 Mbit/s（视频 2.00～2.03 + 音频 0.13～0.14），客户端收到的总字节含协议头。容量点的回环流量：RTMP 8500 约 18 Gbit/s，GB TCP 14000 约 30 Gbit/s，WHEP 3250 约 7 Gbit/s。
- 本测试在 netns 回环上，没有网卡带宽限制。实际部署时，单块 10 GbE（约 9.4 Gbit/s 可用）只能承载约 4300 个 2 Mbit/s viewer，网卡会先于 CPU 成为瓶颈；25 GbE 约 10800 个，除 GB TCP 外都是 CPU 先到上限。容量结论应理解为“服务端 CPU 能处理的上限”。
- 回环让服务端核额外承担接收侧协议处理（softirq），容量点服务端 softirq 1.6～1.8 核（约 20%）。真实网卡部署时这部分移到客户端机器，同时增加网卡驱动发送开销，无法从本测试直接推算净值。
- 回环上有 TCP 重传，但都不是丢包。`551a61c` 之后的轮次记录了原因计数：重传段数与 `TCPLossProbes`（尾部丢包探测，TLP）基本相等，`TCPTimeouts`、`TCPFastRetrans`、`TCPSpuriousRTOs`、`TCPBacklogDrop`、`TCPRcvQDrop`、`TCPZeroWindowDrop`、`PruneCalled` 全为 0，也没有 listen 溢出。TLP 在发送端约 10 ms 内没有收到 ACK 时重发最后一个段，原因是客户端在关键帧突发时处理不及、ACK 延迟。证据：单源 RTMP 3000 有 9559 次，10 源 1861 次，100 源和 300 源为 0（`ms-rtmp3000-s*`）；HLS 14000 有 43583 次（Python 客户端已用满核）；混合 7500 约 2.2 万次。每窗口最多几万次，对应每秒约 1000 个段，在每秒数百万个段中可以忽略，不影响容量结论。
- 服务端 UDP socket 的丢包在 `server_evidence.udp_drops` 中单独统计。只有 UDP 输入出现过丢包（F6）；UDP 输出没有。WHIP 100/150 路时 namespace 的 `UdpRcvbufErrors` 为 526/614，但服务端 socket 丢包为 0，丢包发生在发布工具接收 RTCP/STUN 的 socket 上，不影响服务端媒体测量。

### 6.4 调度、锁与异步队列

- 上下文切换 40～1000 次/s（perf stat，服务端核合计），futex 不在热点中（第 7 节）。`sink_group` 的互斥锁每帧每组加锁一次，没有可见竞争。
- 10 个服务端线程中 8 个 worker 在容量点各 0.94～0.99 核，负载均匀（accept 轮询分配）；另 2 个线程接近 0。
- 真正的问题在异步队列的公平性：分发 handler 不让出（F4），超过饱和点后写 post 无界积压（F5）。它们不降低饱和点以下的吞吐，但决定饱和边缘的控制面可用性和过载行为。

### 6.5 时段漂移

`analysis/notes-drift.md`。同一 ELF、同一客户端、同一负载，10-24 00:00～01:40 的 RTMP/FLV 每 viewer 周期比 04:20 以后高约 25%：FLV 6000 为 3.28 vs 2.59 Mcycles，指令数 1.75 vs 1.81 M，IPC 0.53 vs 0.70。RTSP（02:09 与 06:10）、GB UDP、WHEP 两个时段相同。atop 历史显示期间没有外部负载。漂移只影响访存最密集的两个协议，原因可能在内存子系统（uncore 频率、DRAM 刷新等）。00:30 时的 uncore 状态没有记录，无法确认。`cap2-rtmp` 测得的 6500～6750 与 `cap7-rtmp` 的 8500 都是真实测量。RTMP/FLV 容量应理解为在主机这一不确定性下有约 ±12% 的波动。05:15 起的 envlog 和 7 个哨兵轮次用于检查漂移是否复现（结果见下）。

哨兵结果（RTMP 6500，带 perf stat，与其他任务交替插入队列；`raw/runs/sn*-rtmp-6500`）：

| 轮次 | 完成时间 | 服务端核 | 频率 | Mcycles/viewer·s | Minstr/viewer·s | IPC | PSS |
| --- | --- | --- | --- | --- | --- | --- | --- |
| sn1 | 06:43 | 6.25 | 2.70 GHz | 2.598 | 1.546 | 0.594 | 2207 MiB |
| sn2 | 08:49 | 6.24 | 2.70 GHz | 2.591 | 1.564 | 0.603 | 2199 MiB |
| sn3 | 10:15 | 6.39 | 2.71 GHz | 2.664 | 1.598 | 0.599 | 2191 MiB |
| sn4 | 11:05 | 6.37 | 2.71 GHz | 2.651 | 1.585 | 0.597 | 2248 MiB |
| sn5 | 11:52 | 6.24 | 2.71 GHz | 2.596 | 1.590 | 0.612 | 2148 MiB |
| sn6 | 12:46 | 6.23 | 2.70 GHz | 2.596 | 1.589 | 0.611 | 2088 MiB |
| sn7 | 14:24 | 6.22 | 2.70 GHz | 2.587 | 1.570 | 0.606 | 2240 MiB |

06:43 至 14:24 的 7 个哨兵周期在 2.59～2.66 M 之间（±1.4%），指令 1.55～1.60 M，IPC 0.59～0.61，与 04:20 以后的容量轮次（2.52～2.59）一致，00:00～01:40 那样高 25% 的状态没有再出现。7 个哨兵窗口附近 envlog 的满载样本中 uncore 都是 2.4 GHz，封装 60～75 W，DRAM 10.5～16.1 W，温度 37～47 °C。漂移发生时的主机状态没有记录，原因仍未确定；RTMP/FLV 的容量取自 04:20 以后的重测；其他协议的早期轮次已与 04:20 以后的确认轮次核对，周期偏差超过 5% 时以确认轮次为准。

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

## 8 长稳与资源回收

### 8.1 短时 churn 与清理后回收

`raw/runs/v10-settle`：单源 RTMP 300 个常驻 viewer，另有 50 个 viewer 每 15 s 断开重连，窗口 45 s；客户端全部退出后等待 20 s 再取快照（`--cleanup-settle 20`）。

| 时点 | FD | 线程 | PSS | RSS | VmSize | TCP 收/发队列 | 进程 CPU 累计 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 启动后基线 | 33 | 9 | 19.9 MiB | 21.3 MiB | 227 MiB | 0/0 | 0.04 s |
| 客户端退出后 | 32 | 16 | 89.4 MiB | 91.7 MiB | 457 MiB | 0/0 | 26.72 s |
| 再等 20 s | 32 | 16 | 89.4 MiB | 91.7 MiB | 457 MiB | 0/0 | 26.72 s |

- 会话、套接字和定时器全部回收：FD 回到基线，等待期间进程 CPU 不再增加，没有残留的定时器或重试。
- 多出的 7 个线程来自 Boost.Asio 1.92 的 `resolver_thread_pool`（`boost/asio/detail/impl/resolver_thread_pool.ipp:54-62,113-118`）：每个执行上下文在第一次 `async_resolve` 时启动 1 个解析线程，此后一直保留。服务端每个 worker 的 io_context 在签名校验（`media/http/signaling_verify.cpp:82-85`）时都会解析一次，所以上限是 worker 数 8，不会随会话数增长。
- PSS 比基线多 70 MiB，等待 20 s 没有变化。jemalloc 默认没有后台线程，脏页衰减只在分配调用时推进，进程空闲时不会归还。这是分配器保留，不是泄漏；下一轮负载会复用这些页。jemalloc 衰减参数的 A/B 结果见第 7 节。

### 8.2 30 min 长稳

负载取各协议稳定容量的约 80%～90%，单源，测量窗口 1800 s（warmup 30 s），另有一组 churn viewer 每 20 s 一轮：建立、接收 12 s、断开。窗口结束后等 30 s 再取快照。汇总用 `tools/longsum.py`（每 300 s 一段）。

| 轮次 | 常驻负载 | churn | 结果 | 服务端核（各 300 s 段） | PSS（各段范围） | FD | 实时比例最小 | lag span p50 | 清理后 FD / 线程 / PSS |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `d1-long-rtmp-7000` | RTMP 7000 | RTMP 100 × 86 轮，全部成功 | PASS | 6.78～6.80 | 2.0～2.6 GiB，无上升趋势 | 7033～7141 | 0.9999 | 233～234 ms | 32 / 16 / 2059 MiB |
| `d1-long-whep-3000` | WHEP 3000 | WHEP 50 × 87 轮，均建立并出媒体，但测量屏障失败（工具问题） | FAIL（工具） | 6.45～6.52 | 562～572 MiB | 3033～3084 | 0.9999 | 512～520 ms | 32 / 16 / 495 MiB |
| `d1-long-gb_udp-6400` | GB UDP 6400 | GB UDP 100 × 87 轮（每轮 create/delete 100 个 sender），全部成功 | PASS | 6.12～6.13 | 52～54 MiB | 12833～13035 | 0.9999 | 290～334 ms | 32 / 9 / 48 MiB |
| `d2-long-whep-3000`（`370de80` 重跑） | WHEP 3000 | WHEP 50 × 78 轮，全部成功 | PASS | 6.55～6.63 | 563～578 MiB | 3033～3084 | 0.9999 | 555～559 ms | 32 / 16 / 486 MiB |

- **RTMP 7000**：30 min 内各段服务端 CPU 相差不到 0.4%，PSS 在 2.0～2.6 GiB 间随关键帧和 churn 起伏，没有上升趋势；8600 个 churn 会话全部建立、接收、断开。lag span 233 ms，说明 30 min 内交付延迟没有累积。清理后 FD 回到基线以下（32，基线 33），进程 CPU 不再增加。
- **WHEP 3000**：3000 个常驻 viewer 全程实时，CPU、PSS、FD 平稳。87 轮 churn 的 50 个会话都完成了 ICE/DTLS 并收到媒体（`churn-*.log` 均有 `phase=media_ready ready=50`），但都在测量屏障报 `start-file time is outside the readiness deadline` 后退出：runner 预写的开始时间只比启动晚 4 s，而 WHEP 会话在负载下约 7 s 才全部出媒体。这是工具问题，已由 `370de80` 修正（第 9 节）。用新运行包重跑的 `d2-long-whep-3000` 通过：78 轮共 3900 个 churn 会话全部建立、测量、断开，CPU 除第一段外在 6.60～6.63 核，PSS 与 FD 平稳，清理后 FD 回到 32。两轮合计，服务端在 1 h 内建立并释放了 8250 个 WHEP churn 会话。
- **GB UDP 6400**：8700 次 sender 创建和删除全部成功，FD 只在 churn 的 200 个之间往复，服务端 socket 丢包和 netns `UdpRcvbufErrors` 均为 0。线程始终 9 个：GB 输出不走签名校验，没有启动解析线程。
- **清理后 PSS**：RTMP 2059 MiB、WHEP 486～495 MiB，等待 30 s 不变。原因与第 8.1 节相同：jemalloc 没有后台线程，空闲时不推进衰减，下一轮负载会复用这些页。
- **线程**：RTMP、WHEP 轮为 16 个，与第 8.1 节相同，多出的是每个 worker 的 Asio 解析线程，30 min 内不再增长。

## 9 工具补充与自验证

全部工具修改只涉及 `bench/`、`CMakeLists.txt`（新增 `gb_fanout` 目标）和 `docs/performance.md`，按问题分别提交，未推送：

| 提交 | 内容 | 自验证 |
| --- | --- | --- |
| `1338812` | 原生客户端按视频媒体时间输出 `viewer_realtime_*`，饱和判定；新增 `gb_fanout`（GB UDP/TCP active/TCP passive 输出）及全部输入类型、`--decode-probe` | 1 核负对照 `v1/v3/v4/v5/v6-neg-rtmp-1cpu-*` 判为饱和/落后；正常负载 `v1/v2-*` 全部 realtime ≥0.99 |
| `60a96c4` | runner 套接字队列改为每 5 次采样读一次、复用 PSS；客户端复用 64 KiB 读缓冲；`--churn` | RTMP 6500 原 runner 0.994 核、判 CLIENT_RESOURCE；修正后 runner ≤0.37 核（11000 连接）；`v8-churn-rtmp/gb` 通过 |
| `68a22ae` | 降频采样后 udp_drops 只读取含该字段的样本 | `v9-rtmp-500-d33`、`v9-gb_udp-300-d31` |
| `0418e40` | `--cleanup-settle` 清理后延迟快照 | 见第 8 节 |
| `6ef7c32` | 签名桩记录调用数、拒绝数、处理耗时 | 全部结果的 `verification` |
| `bb16e5c` | `--perf-stat` 统计服务端核 cycles/instructions | 与 APERF/MPERF 周期数逐轮比较，差异 −0.1%～+0.4% |
| `86052ae` | 签名桩零拒绝且最长处理 <1 s 时，准备阶段鉴权失败计入服务端饱和 | WHEP 3750 由 UNCONFIRMED 变为可判定 |
| `c1fd56e` | 清理遇到传输错误不再逐个超时；driver 回收每轮进程组；签名桩 backlog 4096 | `v11-stub-r400`：ListenOverflows 0、桩 p99 0.9 ms，无新的 SYN flooding；`v11-gb_udp-300` 清理 128/128 |
| `14eaecd` | 客户端退出时 `/proc/<pid>/exe` 先消失的竞态 | FLV 6500 原判 BLOCKED，重测见第 4 节 |
| `1c2ff7d` | GB TCP 监听端口与临时端口范围重叠时拒绝运行 | 10500 通过、11000 被拒；原 11000 两轮 `Address already in use` 为工具冲突，作废 |
| `4930da7` | `docs/performance.md` 补充上述选项与判定 | — |
| `49cf28b` | GB TCP 监听端口还须避开服务端与上游实例的 RTMP/RTSP/HTTP 端口 | `--gb-port-base 10000` 跑 2000 个 passive 时直接拒绝（端口 11935 落在范围内）；原 `cap2c-gb_tcp_passive` 因该冲突 create 失败（`14:Bad address`），作废 |
| `f3f5bfb` | 原生客户端记录每个 viewer 窗口内 `墙钟 - 视频媒体时间` 的最大值与最小值之差，输出 `viewer_lag_span_ms_p50/p90/max` | 轻载 100～145 ms（30 s 窗口，含发布端节拍和回环交付抖动）；1 核 GB TCP 1500/1800 为 162/180 ms；RTMP 8500 为 453～876 ms，WHEP 3500 为 863～1037 ms（3250 为 312～410 ms），随饱和单调增大 |
| `c64e7d8` | `--perf-stat` 增加 page-faults、context-switches | RTMP 6500 每秒 21 万次缺页，与 profile 中缺页栈占比一致；上下文切换每秒 40～660 次 |
| `862ee0c` | 就绪超时（`TimeoutError`，是 `OSError` 子类）不再归为 BLOCKED | `cap6c-whep-confirm/round-000-3750` 判为 FAIL/SERVER_SATURATED |
| `c775ca5` | 播放票据有效期由固定 60 s 改为 `--ready-timeout + 60` | 原 `cap6b-whep-confirm` 3750 一组建立 73 s，末尾 2 个 viewer 403；修正后 `cap6c` 两轮签名桩拒绝 0 |
| `cad6ddc` | `docs/performance.md` 补充 lag span、缺页/上下文切换统计、GB 端口检查、就绪超时与票据有效期 | — |
| `939c249` | 发布端改为同时发 SIGTERM、共用 10 s 等待后 kill | ffmpeg 7.1.1 在 RTMP 输出连接未断时不响应 SIGTERM，原来逐个等待 10 s，100 个上游发布端清理约 17 min；本地 7 个进程 10.0 s 内全部停止；测试机上 `032-multisource` 的 7 轮共 522 个 RTMP 发布端（含 300 源一轮），整个作业 12 min 完成（queue.log 08:25～08:37），按原逐个等待方式仅清理就要约 87 min |
| `551a61c` | `namespace_tcp_delta` 增加 TLP、RTO 超时、快速重传、backlog/接收队列/零窗口丢弃、prune 计数 | 之后全部轮次的重传都等于 `TCPLossProbes`，其余计数为 0（第 6.3 节） |
| `370de80` | churn 组改为日志出现 `media_ready` 后再写测量开始时间（原为启动后 ramp+3 s 预写） | `d1-long-whep-3000` 的 87 轮 churn 全部因 `start-file time is outside the readiness deadline` 失败：50 个 WHEP 会话在负载下约 7 s 才全部出媒体（`churn-0.log`）。修正后 `c1-whep-1000-churn`（WHEP 1000 + churn 50）5/5 轮、`c1-rtmp-1000-churn`（RTMP 1000 + churn 100）4/4 轮通过；WHEP 长稳用新运行包重跑（`d2-long-whep-3000`，第 8.2 节） |

外部工具：发布端和解码探测默认使用运行包内的 ffmpeg/ffprobe n7.1.1（`/home/gyl/bin`）。n7.1.1 的 FLV 封装拒绝 Opus（`Audio codec opus not compatible with flv`），它的 ffprobe 也无法识别 Enhanced RTMP 的 FourCC Opus 音频（codec 显示 `unknown`），而服务端 RTMP/HTTP-FLV 输出 Opus 时正是用这种格式。因此 `v4-codec-h264_opus-rtmp` 判为工具缺失。经用户同意，10-24 安装 BtbN 静态构建 ffmpeg n9.0.2-35-g9a9b70bb8c-20261023（本机 `/home/gyl/opt/ffmpeg-n9.0`，测试机 `/var/tmp/ms-perf-891aec1/ffmpeg-n9.0`；ffmpeg sha256 `efe9247b…e663`，ffprobe `e5ddfa26…4301`；下载包已用官方 checksums.sha256 校验），只用于含 Opus 的 `b2-codec-*` 格子：RTMP Opus 发布和 RTMP 拉流用 n9.0 的 ffmpeg 和 ffprobe，RTSP Opus 发布保持 n7.1.1 发布端，只把解码探测换成 n9.0。自验证：在测试机客户端核上用 n9.0 把 H.264/H.265 + Opus 夹具封装成 FLV，再用 n9.0 ffprobe 读出 120/119 个视频帧和 201 个 Opus 帧（4 s）。其他轮次和全部容量轮次的工具版本不变。

工具开销（容量轮次，`analysis/cap2-costs.csv` 及各 `result.json`）：runner 0.12～0.37 核；客户端单核最高 0.73、单线程最高 0.50 核；服务端 SMT 兄弟线程 0.004～0.035 核。没有一轮因客户端或 runner 资源受限。

报告目录 `tools/` 下的分析脚本（不提交）：`pr.sh`/`pc.sh`（netns 内运行 perf_run/perf_capacity）、`prof2.sh`（固定负载下 perf stat、IMC、softirq、perf record、syscount、offcputime）、`costs.py`、`capsum.py`、`profsum.py`、`stackcat.py`（按调用栈把周期归入发送系统调用、softirq、缺页、用户态各类）、`proftable.py`（汇总第 7.2 节分类表）、`matrix.py`（矩阵各组就绪与解码探测）、`sum.py`（单轮判定摘要）、`longsum.py`（长稳轮按 5 min 汇总 CPU、PSS、FD 和 churn）、`assemble.py`（把 `analysis/` 各节拼成 `report.md`）、`offcputime.py`（系统 `offcputime-bpfcc` 的副本，kprobe 改为内核 6.8 的 `finish_task_switch.isra.0`）。

作废数据（`analysis/invalid-rounds.txt`，`tools/capsum.py` 统计时剔除）：`cap2-flv-invalid`、`cap2-rtsp-invalid`、`cap2-gb_tcp_passive-invalid`（上一轮 GB UDP 8000 遗留的服务端占用端口），`cap2-rtsp-invalid2`（运行中替换运行包目录，触发身份校验），`cap2-gb_tcp_active` 的 11000 三轮（012/017/018，端口冲突），`cap2c-gb_tcp_passive-invalid`（端口与 RTMP 11935 冲突），`b2-codec-h264_opus-rtsp_pull`（任务脚本漏配 ffmpeg n9.0，上游无法以 FLV 推 Opus）。

分类修正但保留的轮次：`cap6-whep-confirm/round-003-3750`（原 BLOCKED，实际为就绪超时）和 `cap6b-whep-confirm/round-003/004-3750`（混有票据过期的 403，同时有 STUN 超时），三轮按 FAIL 计。服务端饱和的事实不受工具问题影响，修正后的 `cap6c` 两轮也给出同样结论。

报告目录还有 `tools/envlog.sh`（10-24 05:15～15:05 在 CPU 31 上运行，`nice 10`，测试结束后已停止），每 5 s 记录 uncore 频率、封装/DRAM 功耗、服务端核平均频率和封装温度，写入测试机 `envlog.tsv`（副本在 `raw/env/`），用于核对时段漂移（第 6 节）和功耗、温度（第 2 节）。

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

## 附录 A 方法与判定

- 负载：单源 H.264 CBP 720p30 2 Mbps + AAC 48 kHz（`perf_fixture_720p30.flv`，GOP 2 s），每 viewer 持续接收音视频；编解码矩阵另用 H.265/G.711/Opus fixture（`raw/env/test-host-env.txt` 末尾有 SHA256）。
- 一轮：服务端、上游实例、签名桩由 `perf_run.py` 拉起；viewer 以 200/s 爬坡，全部就绪后 warmup 15 s，测量窗口 60 s。窗口内每秒采样一次进程 CPU/PSS/FD，每 5 秒采一次套接字队列；perf stat 统计服务端核的 cycles/instructions。
- 健康：每组 ready=progressing=请求数、客户端正常退出、窗口覆盖率 ≥0.99、无 UDP drop/队列满、清理无残留，且每个 viewer 的媒体时间推进/墙钟 ≥0.98（`viewer_realtime_min`）。
- 服务端饱和（`SERVER_SATURATED`）：窗口内服务端核平均繁忙 ≥90%（或单核 ≥98%、单线程 ≥95%）且出现实时落后或失败；或准备阶段失败且准备阶段服务端核达到 95%/单核 98%，同时客户端核未饱和、失败不属于客户端资源。鉴权 403 只有在签名桩零拒绝、处理耗时 <1 s 时才计入服务端侧（服务端 3 s 鉴权截止超时）。
- 容量：`perf_capacity.py` 递增、二分，再对健康点重复 3 次、失败点重复 2 次；未能重复的边界另行用固定计数确认，结果按实际通过次数报告。
- 成本口径：schedutil 使频率随负载在 1.0～2.7 GHz 间变化，CPU 时间随频率缩放。跨负载比较用周期数（APERF/MPERF 与 perf stat 交叉核对）和指令数；容量结论以满载（约 2.7 GHz）时的实测为准，不由低负载线性外推。
- 回环口径：服务端与客户端在同一 netns 的 lo 上。服务端发出的包在服务端核的 softirq 中完成接收侧协议处理（UDP/TCP 查找、入队、唤醒客户端 epoll），这部分在真实网卡部署时发生在客户端机器；客户端发往服务端的 TCP ACK 则在客户端核上处理。报告中单列服务端 softirq，作为回环额外成本的上界估计。

## 附录 B 测试中发现的功能问题

只定位、记录和评估影响，未修改生产代码。原始数据位于 `raw/runs/`。

### F1 WHEP 拉流输入的 FLV/RTMP 视频 DTS 以半速推进

现象：

- `v4-in-whep_pull`：同一源的 RTMP、HTTP-FLV viewer 的 `viewer_realtime_min=0.4958`；RTSP、WHEP、GB 为 1.0024。帧率正常：FLV 每 viewer 20 秒收到 601 个视频帧，ffprobe 6 秒解码 181 帧。
- `v5-in-whep_pull`：复现，`realtime_lagging_groups=['rtmp-0','flv-0']`，服务端 CPU 未饱和，判为 FAIL/UNCONFIRMED。
- `d1-whep_pull/decode-flv.log`：FLV 输出 900 帧，pts 跨度 29977 ms，dts 跨度 14834 ms；每 150 帧 dts 只前进 2475 ms（16.5 ms/帧），`pts - dts` 从 6.6 s 增至 21.7 s，持续增长。pts 增量出现 20～47 ms 的跳变。
- 对照 `d1-whip`（WHIP 输入，同一接收实现，但发布端不发 SR）：dts 等于 pts，pts 增量只有 33/34 ms。

根因链：

1. `media/webrtc/whip_media_receiver.cpp:262-287` `apply_sender_report` 每收到一个 RTCP SR，就按 NTP 差值重设 RTP→pts 映射（`rtsp_demuxer_set_timestamp`）。
2. 上游是本服务的 WHEP 输出，SR 由 `third/ireader/librtp/source/rtcp-sr.c:86-89` 生成：SR 中的 RTP 时间戳 = 最后发送包的 RTP 时间戳 + 距该包发送时刻的墙钟差。它反映发送时刻，发送抖动会进入映射，重设后 pts 出现前后跳变。
3. `third/ireader/avbsf/src/avdtsinfer.c:18-24` 把任何小于已见最大 pts 的视频帧视为 B 帧，此后 `max_bframes` 不再归零；`:50-61` 按 `fps1000 = I/P 间隔/(bframes+1)` 推算 dts，结果是 16.5 ms/帧，并钳制 `dts <= pts`。一次足够大的回跳就使该源此后的 dts 永久半速。
4. FLV tag 时间戳取 dts（`media/flv/flv_tag_output.cpp:143-144`），所以 RTMP/HTTP-FLV 的时间线半速推进、CompositionTime 持续增长；RTSP/WHEP/GB 的 RTP 时间戳取 pts，不受影响。

影响评估：

- 范围：WHEP 拉流输入（尤其级联拉取本服务或其他按发送时刻生成 SR 的 WHEP 源）。RTSP 发布 `media/rtsp/rtsp_publish_media.cpp:108-128` 有同样的 SR 重设逻辑，发布端 SR 与媒体时间不一致时也有风险；本轮 ffmpeg RTSP 发布未触发。RTSP 拉流不重设，不受影响。
- 输出：RTMP、HTTP-FLV 直接受影响。HLS 和 GB PS 同样写入 dts（`media/hls/hls_segmenter.cpp:75`），TS/PS 中 PTS-DTS 会无限增大；本轮只验证了 ffprobe 能解码 HLS，未验证播放器长时间播放。
- 时间：FLV CompositionTime 为有符号 24 位毫秒（约 8389 s），按每秒增长 0.5 s 推算，约 4.7 小时后溢出。在此之前，依赖 dts 的播放器、录制和转封装可能出现缓冲异常或时间线错误。
- 严重度：中高。属于功能正确性问题，与性能无关；性能矩阵中该单元格记为 FAIL。

修复方向（待决策，未实施）：只在首个 SR 建立 NTP 对齐、后续不再重设映射；或者无 B 帧源直接令 dts=pts，避免依赖 ireader 的 dts 推断。

### F2 HLS 输出 G.711 使用私有 TS stream type

现象：`v4-codec-h264_pcma-rtmp`、`v4-codec-h264_pcmu-rtmp`、`v4-codec-h265_pcma-rtmp` 中 RTMP/RTSP/FLV 的解码探测都能解出 G.711（`pcm_alaw` 282 帧/6 s），HLS 的音频流被 ffprobe 识别为 `mp3`，解出 0 帧。

原因：`media/hls/hls_segmenter.cpp:295-301` 使用 `PSI_STREAM_AUDIO_G711A/U`（0x90/0x91，GB/T 25724 私有类型，`third/ireader/libmpeg/include/mpeg-proto.h:100-101`）。HLS 规范与通用播放器（Safari、hls.js、ExoPlayer）都不支持 TS 内的 G.711。

影响：G.711 源的 HLS 播放只有视频，音频不可用。Opus 源在 HLS 中被明确跳过（日志 `hls skip unsupported track`），G.711 却输出了不可用的轨。严重度：中（兼容性）。可以和 Opus 一样跳过，或在文档中声明 HLS 不支持 G.711。

### F3 观测与接口语义

- 1 核过载（`v3/v5/v6-neg-rtmp-1cpu-2000`）时，RTMP play 会话因 1 MiB 写队列高水位（`media/net/tcp_transport.cpp:55-60`，`no_buffer_space`）被关闭，但 `media/rtmp/rtmp_session.cpp:45-51` 只调用 `shutdown()`，没有日志。perf_run 的 `no_buffer_space` 证据计数为 0，过载断开只能从客户端 EOF 推断。严重度：低（可观测性）。
- GB sender 创建在源编码不受 PS 支持时（如 Opus）返回 HTTP 500 `operation_failed`（`media/http/gb28181_http.cpp:156-159`），与内部错误无法区分。严重度：低（接口语义）。

### F4 接近饱和时同 worker 的控制面与新会话被媒体分发独占

现象（`raw/runs/diag3-gb_udp-8000`、`raw/diag/diag3-gb_udp-8000/probe.txt`）：

- GB UDP 8000 sender，窗口内服务端 7.44 核、0.5 s 区间最高 7.98 核；全部 viewer 实时（`viewer_realtime_min=0.9969`）。
- 同期对服务端 HTTP 端口的 404 探测：多数 5～300 ms，每 8～12 s 出现 0.8～1.8 s 尖峰；窗口结束 32 组同时删除 sender 时，一次探测 20 s 超时、一次 10.9 s。
- 清理：每组顺序删除，7500 时全部 7500 个删除在 4.4～4.8 s 内完成；8000 时各组在 11～38 s 内只删除 201/8000 个，客户端 10 s 请求超时后停止。删除本身只是摘除登记并 `shutdown()`（`media/http/gb28181_http.cpp:288-294`），开销很小。

根因：`media/core/worker_sink_dispatcher.h` 的 `sink_group::drain()` 在一个 handler 内循环到 `pending` 为空才返回（`for (;;)`）。worker 媒体负载接近 100% 时，新帧到达速度接近处理速度，这个 handler 会连续运行很久，同一 io_context 上的其他 handler（HTTP 请求、accept、播放鉴权回调、其他会话读写）都要等它让出。HTTP 连接按轮询分到各 worker（`service.cpp:44-69`），所以控制请求的延迟由所在 worker 的分发积压决定。

影响：

- 媒体面在 8000 时仍实时，但控制面不可用，按服务端饱和计 FAIL；GB UDP 容量取 7500 一侧。
- WHEP 3750（`raw/capacity/cap6b-whep-confirm/round-003/004`）：爬坡时服务端核 100%，一组 256 个 viewer 的建立耗时 73 s，末尾 viewer 出现 `STUN nomination timeout`。新会话的 ICE/DTLS 处理排在饱和 worker 的分发之后。
- GB TCP active 14000（`raw/capacity/cap2c-gb_tcp_active/round-006-14000`）与 1 核 GB TCP passive 2500（`raw/runs/v12-neg-gb_tcp-1cpu-2500`）：爬坡时服务端核 100%，最后一组的 `/gb28181/sender/create` 超过客户端 10 s 请求时限（客户端记为 `14:Bad address`），服务端日志没有创建失败记录。媒体面并未失败，失败的是控制请求。
- 正常负载（≤ 85%）下不明显；它决定的是饱和边缘的行为，而不是吞吐上限。

方向（未实施）：`drain()` 每次只处理一批，仍有积压时重新 `post` 自身，把执行权交还 io_context。不改变分发模型和 D03/D06 的所有权，只增加每批一次 post。它不能提高吞吐，只能让控制面和新会话在饱和边缘得到公平调度。

### F5 饱和后同 worker 的 TCP viewer 集体断开，断开前内存膨胀

现象：

- `raw/capacity/cap7-rtmp/round-004-9000`：9000 个 RTMP viewer 全部就绪后，窗口内 2217 个收到服务端 EOF。36 个客户端组各断开 61～67 个（约 1/4），viewer 按 accept 轮询分到 8 个 worker，相当于 2 个 worker 上的 viewer 全部断开。服务端 PSS 峰值 13.3 GB（8500 健康轮次为 4.1～5.1 GB），日志没有任何断开记录。
- RTSP 8250/8500（`raw/capacity/cap2b-rtsp/round-007/008`）同样表现为服务端主动 EOF、PSS 11～13 GB。
- 多源同样发生：`raw/capacity/cap3-rtmp-s100/round-008-9000`、`round-009-8750`（100 个源）各组 77～113/256 个 viewer 被服务端 EOF，PSS 13.6/12.3 GB；`cap3-mix/round-010-8000` 在测量前 RTMP、RTSP 各组即出现 EOF，`round-011-7750` RTSP 每组 63～64 个断开，同一轮 WHEP 和 GB 组不受影响。

机制（由代码推断，未逐 viewer 打时间戳证实）：

1. `sink_group::drain()`（`media/core/worker_sink_dispatcher.h:183-217`）在饱和 worker 上长时间不返回（见 F4）。
2. drain 对每个 viewer 调用 `tcp_transport::write()`（`media/net/tcp_transport.cpp:26-37`），它只是复制数据并 `post` 到同一 worker。drain 不返回，这些 handler 就留在 io_context 队列里；`queued_write_bytes_` 在 `safe_write` 执行时才累加，所以排队数据不受 1 MiB 高水位约束，PSS 随积压线性增长。
3. drain 让出后，积压的 `safe_write` 依次执行，同一 viewer 很快超过 1 MiB，回调 `no_buffer_space`，`rtmp_session` 静默 `shutdown()`；或者跨 worker 的 `pending` 超过 2500 帧（`worker_sink_dispatcher.h:84-89,154`），整个 sink group 被 `on_end()`。两条路径都没有日志，按 worker 成批发生。

对照：GB TCP sender（`media/gb28181/gb28181_tcp_sender_session.cpp:117-135`）把数据追加到会话自己的 `output_`，只在 `output_` 为空时 post 一次 flush，积压期间多帧合并为一次写。饱和时它的每 viewer 周期从 2.6 降到 1.55 Mcycles，viewer 不断开、只是延迟增大（`cap2c-gb_tcp_active`、`cap2d-gb_tcp_passive`）。

影响：RTMP/RTSP/HTTP-FLV 超过饱和点后不是逐步降级，而是一个 worker 上的所有 viewer 同时断开，断开前占用数 GB 内存。在 `--max-server-pss-mib` 较小或与其他服务共存的部署中，这段膨胀可能先触发 OOM。严重度：中（过载行为和可观测性），不影响饱和点以下的正常运行。

### F6 UDP 媒体输入使用系统默认接收缓冲，多源时关键帧突发丢包

现象：

- `raw/runs/in-gb_udp-300`：300 路 GB UDP 输入，每源 1 个 FLV viewer。服务端只用 2.41 核（30%，单核最高 0.35），窗口内服务端 UDP socket 丢包 204 个（`server_evidence.udp_drops`，每 5 s 采样从 156 匀速增至 360，爬坡阶段已有丢包），判 SERVER_UNSTABLE。同样配置 100 路（`in-gb_udp-100`）丢包 0。
- `raw/runs/in-whep_pull-100`：100 路 WHEP 拉流输入（SRTP over UDP），服务端 5.90 核、单核最高 0.75，丢包 813。
- `in-rtsp_udp-100`：100 路 RTSP UDP 发布，丢包 0。
- H.265 + AAC 经 GB UDP 输入、同时带全部 8 种输出（每种 5～20 个 viewer）：4 轮中 2 轮 HLS 首个分片 10 s 内未生成（`hls playlist not ready`，503），runner 中止（`raw/runs/b2-codec-h265_aac-gb_udp`、`-r3`）；另 2 轮通过（`-r2`、只开 HLS 的 `-hlsonly`）。两轮失败的 netns 级 `UdpRcvbufErrors` 整轮增量为 110 和 108，两轮通过为 0；同一夹具经 GB TCP active/passive 输入、H.264 夹具经 GB UDP 输入都没有丢包。H.265 夹具关键帧平均 132 KB、最大 142 KB（H.264 夹具平均 97 KB），约 100 个 RTP 包，丢包数正好约等于一个关键帧。客户端 UDP socket 都设置了大接收缓冲（`bench/clients/gb_client.cpp:200`），所以推断丢包发生在服务端 GB UDP 接收 socket 上：H.265 关键帧突发超过了 212992 字节的默认缓冲。HLS 分片要从关键帧开始（`media/hls/hls_segmenter.cpp:43-49`），播放列表等待 10 s 仍无分片时返回 503（`media/http/hls_http_session.cpp:144-160`），HLS 客户端因此退出；runner 随即中止，其他输出组还没进入测量，无法判断它们是否受影响。为什么一个关键帧丢失就让首分片超过 10 s，本轮没有逐帧追踪，原因未完全确认。
- 带采样的诊断轮（`raw/runs/p2-in-whip-100`、`p2-in-gb_udp-200`，开着 perf record、syscount、offcputime）：WHIP 100 路丢包 244、GB UDP 200 路丢包 12，服务端分别只用 5.35 和 1.78 核；同配置不采样的 `in-whip-100` 丢包为 0。很小的额外延迟就足以溢出，说明默认接收缓冲几乎没有余量。
- WHIP 输入梯度（`in-whip-100/150/200`）：100、150 路丢包 0；200 路丢包 98783，FLV viewer 在测量前因断流失败（判 SERVER_UNSTABLE）。200 路时 8 个 worker 线程各 0.72～0.87 核，服务端 12.7 Gcycles/s（2.08 GHz），只有满载能力 21.6 G 的 59%。指令数随源数线性增长（每源 134～141 Minstr/s），CPU 时间却维持在 5.7～6.0 核，schedutil 只把频率升到 1.2～2.1 GHz。

原因（推断）：

- `media/net/udp_transport.cpp:16-40` 打开并绑定 socket，没有设置 `SO_RCVBUF`，接收缓冲取主机默认的 `net.core.rmem_default = 212992`。
- 测试素材关键帧平均 88 KB、最大 118.5 KB（ffprobe），约 85 个 RTP 包。回环 skb 的 truesize 每包约 2.3～2.5 KB，一个关键帧突发就接近 208 KiB 上限。worker 逐包经 reactor 完成读取，只要读取稍慢于突发到达（例如正在处理其他源的帧），就会溢出；worker 平均只有 35% 繁忙也会发生。源越多（每 worker 37 个源对 12 个），突发与处理重叠的概率越大，所以 300 路丢包而 100 路不丢；WHIP 和 WHEP 拉流每源还要在 worker 上同步做 SRTP 解密和 Opus→AAC 转码（每个 20 ms 音频帧一次），worker 被成段占用，接收更容易落后：WHEP 拉流 100 路、WHIP 200 路即出现丢包，远在 CPU 饱和之前。
- 没有在本轮做 `SO_RCVBUF` 或 netns `rmem_default` 的对照（前者要改生产代码，后者要改测试环境配置），机制未直接证实。

影响：关键帧丢包会造成解码花屏，直到下一个关键帧（本素材 GOP 2 s）。真实网卡同样会成串到达关键帧突发，主机默认 `rmem_default` 也常为 208 KiB，因此不是回环特有现象。多路 GB UDP/RTSP UDP/WHIP/WHEP 拉流接入时会遇到。严重度：中。

方向（未实施）：为媒体接收 socket 设置较大的 `SO_RCVBUF`（例如 2～4 MiB，受 `rmem_max` 限制），配合 `udp_drops` 观测验证。属于 transport 内部的 socket 选项，符合 D04 的网络所有权划分。
