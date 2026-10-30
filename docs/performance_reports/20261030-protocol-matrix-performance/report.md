# 全协议性能矩阵与 WHIP 跨协议输出（2026-10-30）

## 结论

本轮只测量、不优化：没有修改生产代码，被测 `media_server` 与 `92850b4` 的生产源码一致。

- 功能矩阵 31/31 PASS。工具层修复了 B 帧 WHEP 回放和 G.711 WHEP 回放，原有测试都没有覆盖这两项。
- WHIP→8 种输出的全部 51 个摸底点均 PASS，无 UDP 丢包，全部观看者实时推进。

**按路径的判断**

| 路径 | 判断 | 依据 |
| --- | --- | --- |
| WHIP→RTMP / HTTP-FLV / RTSP / GB TCP | GO | 每观看者 1.3–1.5 Mcycles/s，几乎全是协议必需的 TCP 发送；每源 Opus→AAC 只做一次 |
| WHIP→HLS | GO | 每观看者 0.4 Mcycles/s；内存随源数增长（每源约 4.5–5.7 MiB 分片缓存） |
| WHIP→GB UDP | GO | 每观看者 2.4 Mcycles/s，几乎全部是逐包 `sendto` |
| WHIP→WHEP（直通） | GO（转发）/ 待决策（按需 AAC） | 每观看者 5.2 Mcycles/s，其中 SRTP 与逐包 `sendto` 是必需开销。但每源仍固定执行 Opus→AAC（约 23 Mcycles/s，占每源成本 2/3），纯 WHEP 时这部分无人消费 |
| WHIP→WHEP（Chrome 默认 offer） | OPTIMIZE 候选，需决策 | Chrome 默认 offer 不带 `stereo=1`，因此必走 AAC→Opus egress：每源再加约 20 Mcycles/s，形成 Opus→AAC→Opus 双重转码。Chrome WHIP 推来的实为单声道 Opus，却按 2 声道登记 |
| WHIP→全部输出 | GO | 64 源 336 观看者实测：每源 1 个 Opus→AAC，无重复转码器；成本与各路径之和一致 |
| 其他输入×编码×输出 | GO | 15 种输入编码组合全部 PASS；不支持的组合均为文档已列的编码限制 |
| Firefox 端到端解码 | BLOCKED | 无头 Firefox 内容进程在 WebRTC 接收时 SIGSEGV；offer 与服务端直通选择已验证，客户端解码未验证 |

**WHIP 容量（8 核，每源 2 个观看者，单一输出协议）**

| 输出 | 最大持续：源 / 观看者 | 留余量稳定（最忙核 ≤0.87） | 主要成本 |
| --- | --- | --- | --- |
| RTMP、HTTP-FLV、RTSP、GB UDP、GB TCP 主动、GB TCP 被动 | 480 / 960（各 2/2 PASS；512 FAIL） | 416 / 832（0.83–0.85） | 每源 Opus→AAC |
| HLS | 544 / 1088（2/2；576 FAIL） | 480 / 960（0.87） | 每源 Opus→AAC；PSS 3.1 GiB |
| WHEP（直通，Firefox 类 offer） | 384 / 768（2/2；416 为 1/2；448 FAIL） | 352 / 704（0.86） | 每源 Opus→AAC 加 SRTP 与逐包发送 |
| WHEP（Chrome 默认 offer） | 256 / 512（2/2；288、320 FAIL） | 224 / 448（0.84） | 再加每源 egress |
| 全部输出同时（每源 5.25 观看者） | 352 / 1848（2/2；384 FAIL） | 288 / 1512（0.82） | 同上之和 |

各路径在最大持续点的 cycles 都是 17.2–18.3 Gcycles/s，失败点都在 18.4 以上，属于 CPU 计算量受限。超出边界时，首先表现为 WHIP 输入的 UDP 接收丢包。

**需要决策**
1. 是否实施按需 Opus→AAC。纯 WHEP 直通或无观看者的 WHIP 源可省约 2/3 的每源 CPU；但只要源上有 AAC 类观看者或 Chrome 观看者，就省不下来，而且会引入转码器启停的生命周期状态。
2. 是否让 WHIP 源按实际 Opus 声道（TOC stereo 位）登记。这能让 Chrome 观看 Chrome 推流时直通，省去每源约 20 Mcycles/s 的 egress，并避免一次有损重编码；但需要运行时检测状态，FFmpeg WHIP 推流（真立体声）不受影响。
3. 是否把 WHEP 输出加入矩阵的 H.265 场景（本轮手动补测已 PASS），以及是否给 GB 输出补解码与音画同步探测。

详见第 17、20 节。

## 1. 版本与构建

| 项 | 值 |
| --- | --- |
| 仓库 HEAD | `92850b4`（工作区仅 `bench/`、`docs/` 改动，见“文件”） |
| 被测二进制 | `media_server` SHA256 `b95752baab1bc67d9aafdb4ff0016c97afbb4b107ff3cc3dba7d177e99d732d3`，自干净 `3b6c0bd` 构建；`git diff 3b6c0bd 92850b4 -- media third deps CMakeLists.txt cmake` 为空 |
| `--version` | `media_server 3b6c0bdcfd65 compiler 16.0.1 20260315 (experimental) boost 1_92 OpenSSL 3.5.9 ffmpeg n9.0.1 libopus 1.6.1 libsrtp2 2.7.0 allocator jemalloc 5.3.0` |
| 子模块 | ada v4.0.0、fdk-aac v2.0.3、ffmpeg n9.0.1、libsrtp v2.7、opus v1.6.1、simdjson v4.6.11、spdlog v1.17.0 |
| 压测客户端 | 工作区构建：`whep_fanout` `3a0c0302…`（新增 `--opus-fmtp`）、`whip_publisher` `822131b8…`；其余客户端同上一轮 |
| Sanitizer | `media_server-asan-head` `efaf825f…`、`media_server-tsan-head` `4fdba9ea…`，均自 `3b6c0bd` 构建 |

## 2. 测试环境

- 测试机：Intel Xeon Silver 4216（32 逻辑 CPU），93 GiB 内存，内核 6.8.0-84，`schedutil` 调频（800–3200 MHz）。
- CPU 分配：
  - 服务端固定 CPU 0–7，8 个 worker；SMT 兄弟 16–23 保持空闲（每次运行前检查 <0.05）；
  - 客户端与上游使用 8–15、24–31。
- 素材：与上一轮相同。
  - WHIP 源：`perf_fixture_whip.mkv`，H.264 720p30 加 Opus 48 kHz 立体声。
  - RTMP 源：`perf_fixture_720p30.flv`，H.264 Constrained Baseline 720p30 2 Mbit/s 加 AAC-LC 48 kHz 立体声 128 kbit/s。
  - 派生素材（H.265、PCMA、PCMU、Opus-in-FLV、音画同步标记）由矩阵脚本生成。
- 测量窗口：预热 15 s，测量 60 s，`perf stat` 覆盖整个窗口，控制面探测每 200 ms 一次。
- 资源门槛：空闲内存 ≥8 GiB、磁盘 ≥3–4 GiB、服务端 PSS ≤16 GiB。
- CPU 计量：`schedutil` 使“忙核数”随频率漂移，成本一律用 `perf stat` 的 cycles/s。8 核满载约 18.6–19.1 Gcycles/s（上一轮实测）。

## 3. 网络命名空间与临时端口

[env_check.sh](env_check.sh) 在每次运行前执行，失败记为 BLOCKED，不计入服务端结果。

| 项 | 值 |
| --- | --- |
| 命名空间 | `msperf-20261013T011210Z-7788320`，DUT `198.19.0.10`，客户端 `198.19.0.11`（同在 lo） |
| 命名空间内 `ip_local_port_range` | `32768 49151`（脚本在命名空间内设置并回读） |
| 宿主机 `ip_local_port_range` | `32768 60999`（未改动） |
| media_server 媒体端口池 | `49152 65535`（生产默认，未改动）；与临时端口不重叠 |
| 其他检查 | 固定端口 11935/18554/18080/12035/18654/18180 无监听；无残留压测进程；命名空间内非 TIME-WAIT socket <64；`nofile` 可提升到 1048576 |

本轮共写出 97 份环境记录，其中 3 份 BLOCKED，均为环境原因，已重排运行：
- `lc-smoke`：浏览器测试的 DUT 仍在运行；
- `lc-smoke2`：`pgrep -f` 误匹配 bpftrace 的命令行，已改为按进程名精确匹配；
- `w-whep-a`：上一轮刚结束时 CPU 仍为 0.109 忙，已改为重试等待。

全部运行没有出现临时端口耗尽、socket 创建失败或 GB 端口冲突。客户端资源见各表的 client cores 列：最忙客户端线程 ≤0.7 核，未成为瓶颈。

## 4. 功能矩阵（31 项）

运行：`regression/protocol_matrix.py`，Release，4 worker，服务端 CPU 0–3。

| 结果 | 数量 | 说明 |
| --- | --- | --- |
| 媒体场景 PASS | 20/20 | 全部组就绪；RTMP/RTSP/FLV/HLS 解码探测通过；A/V 探测通过；FD 回到基线；无残留 receiver |
| 预期失败 PASS | 11/11 | 源停止，以及 rtmp/rtsp/flv/hls/whep 的无效 token、重放 token 均按预期拒绝 |

逐项表见附录 A。

**`rtmp-h264-bframes-aac` 首次 FAIL**
- 原因：WHEP 回放录制器以 `-c copy` 写 mkv，B 帧经 RTP 到达时 DTS 非单调，录制器退出码 234。这是测试工具问题。
- 修复：[`bench/whep_dump_av.py`](../../../bench/whep_dump_av.py) 检测到 RTP 时间戳重排时改为重新编码视频；“回退”只统计低于已见最大时间戳 0.5 s 以上的包。
- 复跑：PASS，WHEP 偏差 3.7 ms。
- 上一轮矩阵工具没有 WHEP A/V 探测，这一缺口此前未暴露。

**G.711→WHEP 的音频此前未被验证**
- 回放工具只识别 Opus（PT 111），PCMA/PCMU 场景的 WHEP 录像没有音轨，`av_markers=0` 却判为 ok。
- 已让工具识别静态 PT 8/0 并录为 PCM，离线重放结果见第 15 节。

## 5. 输入/编码/输出覆盖表

PASS 依据：小规模矩阵加 16 源统一摸底（第 8 节）。

| 输入 \ 输出 | RTMP | RTSP | HTTP-FLV | HLS | WHEP | GB UDP | GB TCP 主动/被动 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| RTMP H.264+AAC（含 B 帧） | PASS | PASS | PASS | PASS | PASS（AAC→Opus egress） | PASS | PASS |
| RTMP H.264+Opus | PASS | PASS | PASS | PASS（仅视频） | PASS（原生 Opus） | UNSUPPORTED | UNSUPPORTED |
| RTMP H.264+PCMA/PCMU | PASS | PASS | PASS | PASS（仅视频） | PASS（原生 G.711） | PASS | PASS |
| RTMP H.265+AAC | PASS | PASS | PASS | PASS | PASS（egress；本轮补测，矩阵未覆盖） | PASS | PASS |
| RTSP TCP/UDP H.264+AAC | PASS | PASS | PASS | PASS | PASS（egress） | PASS | PASS |
| RTSP TCP H.264+Opus | PASS | PASS | PASS | PASS（仅视频） | PASS | UNSUPPORTED | UNSUPPORTED |
| RTSP TCP H.264+PCMA | PASS | PASS | PASS | PASS（仅视频） | PASS | PASS | PASS |
| RTSP TCP H.265+AAC | PASS | PASS | PASS | PASS | PASS（egress；本轮补测，矩阵未覆盖） | PASS | PASS |
| WHIP H.264+Opus | PASS | PASS | PASS | PASS | PASS（直通或 egress，见第 14 节） | PASS | PASS |
| RTMP Pull / RTSP Pull（AAC） | PASS | PASS | PASS | PASS | PASS（egress） | PASS | PASS |
| WHEP Pull（Opus，转 AAC） | PASS | PASS | PASS | PASS | PASS（直通） | PASS | PASS |
| GB UDP / TCP 主动 / TCP 被动（AAC） | PASS | PASS | PASS | PASS | PASS（egress） | PASS | PASS |

UNSUPPORTED 均为 [protocols.md](../../protocols.md) 已列的编码限制：
- GB PS sender 只接受 AAC/G.711 音频，非 WHIP 的 Opus 源没有 AAC 主流可送；
- HLS 的 MPEG-TS 不承载 Opus/G.711，只输出视频。

本轮没有为这些组合更换素材。

**H.265→WHEP**
- 矩阵的两个 H.265 场景使用 `NO_WHEP_OUTPUTS`，从未覆盖 WHEP 输出。但代码会在观众 offer 的 H265 profile/tier/level 与源匹配时协商 H.265。
- 本轮用 `whep_fanout`（offer 含 H265 Main、level-id 186）补测：RTMP 与 RTSP TCP 的 H.265+AAC 源 → WHEP 均 PASS，A/V 探测通过。
- 浏览器的 H.265 WebRTC 解码依赖平台硬件，未验证。

## 6. WHIP→各输出独立结果

每条路径三种形态，Release，8 worker：
- a：1 源 4 观看者；
- b1/b2：4 源，WHEP 200/400，其余 TCP 500/1000，GB UDP 200/400，HLS 与 GB TCP 100/200；
- c1/c2：32/64 源，每源 2 观看者。

完整表见附录 B，指标定义见 [costs.py](costs.py)。

**每观看者与每源成本**（Mcycles/s，由 b2−b1、c2−c1 差分得到）

| 输出 | 每观看者 | 每观看者指令 (M/s) | 每源（含其 2 观看者） | 每源（扣除观看者） |
| --- | --- | --- | --- | --- |
| 无输出 | — | — | 35.6 | 35.6 |
| WHEP（直通） | 5.2 | 6.9 | 43.3 | 33.0 |
| WHEP（Chrome offer，egress） | 5.1 | 6.9 | 63.5 | 53.4 |
| RTMP | 1.4 | 1.5 | 38.9 | 36.0 |
| HTTP-FLV | 1.5 | 1.7 | 38.3 | 35.3 |
| RTSP | 1.5 | 1.9 | 39.2 | 36.1 |
| HLS | 0.4 | 0.2 | 35.2 | 34.3 |
| GB UDP | 2.4 | 2.8 | 39.0 | 34.2 |
| GB TCP 主动 | 1.4 | 1.6 | 39.2 | 36.4 |
| GB TCP 被动 | 1.3 | 1.5 | 36.3 | 33.6 |

**每源成本**
- 扣除观看者后为 33–36 Mcycles/s，与输出协议无关，主要是每源固定的 Opus→AAC（第 13 节）。
- 各路径之间的差异在 ±1.5 Mcycles/s 内，属于差分噪声。
- Chrome offer 例外：每源多约 20 Mcycles/s，即一个 AAC→Opus egress。每观看者成本与直通相同（5.1 对 5.2），说明 egress 按源共享。

**每观看者成本**
- WHEP 最高：SRTP 加密，且每个 RTP 包一次 `sendto`。
- GB UDP 次之：逐包 `sendto`，无加密。
- TCP 类约 1.4：asio 批量写，最多 64 段一次 `sendmsg`。
- HLS 最低：HTTP 分片由客户端拉取，分片只封装一次。

**其他观测**
- 带宽：每观看者稳态约 2.13–2.17 Mbit/s；b2 形态输出 0.87–2.17 Gbit/s。
- 内存：
  - 每 TCP 观看者约 0.25 MiB（RTSP b2 1000 观看者 PSS 309 MiB）；
  - 每 WHEP 观看者约 0.2 MiB；
  - GB UDP 400 观看者只有 36 MiB；
  - HLS 每源约 4.5–5.7 MiB 分片缓存（64 源 403 MiB，544 源 3.1 GiB）。
- 控制面：
  - 大多数点 p99 < 10 ms；
  - WHEP b1/b2 的 p95/p99 为 7.4/27.8 ms、10.0/47.7 ms；RTMP b2、GB UDP b2 的 p99 为 19.1/26.6 ms，均无错误。
  - 少源多观看时，控制请求与同一 worker 上的批量 fanout 竞争。上一轮 B 混合的 p99 为 4.4 ms，两者负载形态不同，不可直接比较。
- TCP 重传：
  - 只在 1000 TCP 观看者时出现，FLV 1070、RTSP 131、RTMP 96。
  - 均为环回上的尾部丢失探测，观看者未掉线，实时比 ≥0.999。

## 7. WHIP→多协议同时输出

- 输出组合：每源 1 WHEP、1 RTMP、1 FLV、1 RTSP、¼ HLS、½ GB UDP、¼ GB TCP 主动、¼ GB TCP 被动。
- 形态：a 为 1 源 24 观看者；b 为 4 源 675 观看者；c 为 64 源 336 观看者。

| 运行 | Gcycles/s | IPC | 最忙核 | PSS MiB | 输出 Mbit/s | 结果 |
| --- | --- | --- | --- | --- | --- | --- |
| w-all-a | 0.21 | 0.78 | 0.05 | 37 | 52 | PASS |
| w-all-b | 1.79 | 1.09 | 0.27 | 155 | 1451 | PASS |
| w-all-c | 3.63 | 1.40 | 0.54 | 226 | 723 | PASS |
| w-allchrome2-c（WHEP 用 Chrome offer） | 4.83 | 1.52 | 0.71 | 283 | 721 | PASS |

**线性检验（w-all-c）**
- 64 源 × 35.6 = 2.28 G；
- 336 个观看者按各自路径的每观看者成本相加约 0.76 G（WHEP 64×5.2 + RTMP/FLV/RTSP 192×1.45 + GB UDP 32×2.4 + GB TCP 32×1.35 + HLS 16×0.4）；
- 合计约 3.0 G，实测 3.63 G。
- WHEP 换成 Chrome offer 后为 4.83 G，多出的 1.20 G 正是 64 个 egress（每个约 18.8 M）。
- 剩余约 0.6 G 来自全部输出同时存在时更多的会话、定时器和跨 worker 投递（`kernel sched/syscall/epoll` 0.35 G、`asio` 0.16 G，高于单路径 c2 的 0.22/0.07–0.12 G）。

**共享检验（bpftrace，第 11 节）**
- 64 源只有 64 个 Opus→AAC 转码器，无 egress；
- WHEP 观众全部命中直通；
- 各协议观看者共享同一 AAC 主流与视频帧，没有重复的封装或转码实例。

## 8. 其他输入组合

每个组合 16 源，各测两次：
- “none”：只有探测观众；
- “all”：每源 2 RTMP、2 RTSP、2 FLV、½ HLS、2 WHEP、1 GB UDP、½ GB TCP 主动、½ GB TCP 被动；Opus/H.265 源去掉不支持的输出。

完整表见附录 C。

| 输入组合 | none：Mcycles/s 每源 | all：Gcycles/s | all 中的转码 |
| --- | --- | --- | --- |
| RTMP H.264+AAC | 15（首次 26 未复现，见第 18 节） | 1.23 | 每源 1 个 AAC→Opus egress（供 WHEP） |
| RTSP TCP / UDP H.264+AAC | 12.5 / 12 | 1.05 / 1.01 | 同上 |
| RTMP Pull / RTSP Pull（AAC） | 12.5 / 13 | 1.36 / 1.38 | 同上 |
| GB UDP / TCP 主动 / TCP 被动（AAC） | 15 / 14 / 14 | 1.39 / 1.41 / 1.36 | 同上 |
| RTMP / RTSP TCP H.264+Opus | 15 / 19 | 0.64 / 0.73 | 无（FLV/RTMP/RTSP 原生承载 Opus，WHEP 原生 Opus） |
| RTMP / RTSP TCP H.264+PCMA | 15 / 16 | 0.76 / 0.81 | 无（WHEP 与 GB 原生 G.711） |
| RTMP H.265+AAC（无 WHEP） | 15 | 0.49 | 无 |
| RTMP H.265+AAC，只有 32 WHEP | 15 | 0.81 | 每源 1 个 egress |
| WHIP H.264+Opus | 49 | 1.37 | 每源 1 个 Opus→AAC；WHEP 直通 |
| WHEP Pull（AAC 上游经 Opus 拉入） | 51 | 1.35 | 每源 1 个 Opus→AAC；WHEP 直通 |

- 不需要转码的输入：每源 12–19 Mcycles/s。
- 需要转码的 WHIP/WHEP Pull：每源 49–51 Mcycles/s。
- 带 WHEP 观众的 AAC 源：每源多 1 个 egress，约 20 Mcycles/s。
- 音频转码是输入侧唯一显著的差异。
- 视频编码（H.264/H.265）对服务端成本影响很小，服务端只解析、不转码视频。

## 9. 最大持续容量与留余量稳定容量

- 搜索方式：固定每源 2 个观看者，从 320 源起每步 +64，失败后二分一次；最高通过点重复一次。另补测 416/480/352/224/288 作为留余量点。
- WHEP 类观众按 128 一组启动，避免单个 `whep_fanout` 热线程过载。全部点的客户端最忙线程 ≤0.52 核。
- 判定条件与上一轮相同：
  - 全部观看者就绪并持续推进；
  - 实时比 ≥0.98（实际 ≥0.999）；
  - 无 UDP 接收丢包；
  - 资源回收正确。

| 路径 | 源 / 观看者 | 结果 | Gcycles/s | 最忙核 | PSS MiB | UDP 丢包 |
| --- | --- | --- | --- | --- | --- | --- |
| RTMP | 416 / 832 | PASS | 15.66 | 0.84 | 690 | 0 |
| RTMP | 448 / 896 | PASS | 16.62 | 0.90 | 750 | 0 |
| RTMP | 480 / 960 | 2/2 PASS | 17.67–17.68 | 0.96 | 801–805 | 0 |
| RTMP | 512 / 1024 | FAIL | 18.42 | 1.00 | 951 | 216134 |
| HTTP-FLV | 416 / 832 | PASS | 15.89 | 0.85 | 602 | 0 |
| HTTP-FLV | 480 / 960 | 2/2 PASS | 17.85–17.87 | 0.97 | 695 | 0 |
| HTTP-FLV | 512 / 1024 | FAIL | 18.42 | 1.00 | 850 | 331369 |
| RTSP | 416 / 832 | PASS | 15.86 | 0.85 | 674 | 0 |
| RTSP | 480 / 960 | 2/2 PASS | 17.79–17.85 | 0.97 | 769 | 0 |
| RTSP | 512 / 1024 | FAIL | 18.42 | 1.00 | 928 | 301210 |
| GB UDP | 416 / 832 | PASS | 15.82 | 0.85 | 555 | 0 |
| GB UDP | 480 / 960 | 2/2 PASS | 17.76–17.80 | 0.96 | 639–644 | 0 |
| GB UDP | 512 / 1024 | FAIL | 18.47 | 1.00 | 782 | 304537 |
| GB TCP 主动 | 416 / 832 | PASS | 15.64 | 0.84 | 634 | 0 |
| GB TCP 主动 | 480 / 960 | 2/2 PASS | 17.51–17.53 | 0.95 | 725 | 0 |
| GB TCP 主动 | 512 / 1024 | FAIL | 18.40 | 1.00 | 825 | 8573 |
| GB TCP 被动 | 416 / 832 | PASS | 15.52 | 0.83 | 635 | 0 |
| GB TCP 被动 | 480 / 960 | 2/2 PASS | 17.52–17.57 | 0.95 | 730 | 0 |
| GB TCP 被动 | 512 / 1024 | FAIL | 18.40 | 1.00 | 879 | 48901 |
| HLS | 480 / 960 | PASS | 16.18 | 0.87 | 2721 | 0 |
| HLS | 512 / 1024 | PASS | 17.05 | 0.92 | 2902 | 0 |
| HLS | 544 / 1088 | 2/2 PASS | 18.17 | 0.99–1.00 | 3106–3114 | 0 |
| HLS | 576 / 1152 | FAIL | 18.42 | 1.00 | 3263 | 832280 |
| WHEP 直通 | 320 / 640 | PASS | 14.75 | 0.79 | 614 | 0 |
| WHEP 直通 | 352 / 704 | PASS | 16.11 | 0.86 | 673 | 0 |
| WHEP 直通 | 384 / 768 | 2/2 PASS | 17.17–17.20 | 0.92 | 738–740 | 0 |
| WHEP 直通 | 416 / 832 | 1/2 PASS | 18.47–18.48 | 1.00 | 793–820 | 0、9718 |
| WHEP 直通 | 448 / 896 | FAIL | 18.66 | 1.00 | 976 | 556019 |
| WHEP Chrome offer | 224 / 448 | PASS | 15.50 | 0.84 | 640 | 0 |
| WHEP Chrome offer | 256 / 512 | 2/2 PASS | 17.38–17.39 | 0.94 | 737–743 | 0 |
| WHEP Chrome offer | 288 / 576 | FAIL | 18.48 | 1.00 | 881 | 281385 |
| WHEP Chrome offer | 320 / 640 | FAIL | 18.53 | 1.00 | 1013 | 933961 |
| 全部输出 | 288 / 1512 | PASS | 15.37 | 0.82 | 956 | 0 |
| 全部输出 | 320 / 1680 | PASS | 16.81 | 0.90 | 1045 | 0 |
| 全部输出 | 352 / 1848 | 2/2 PASS | 18.29–18.30 | 0.99 | 1142–1150 | 0 |
| 全部输出 | 384 / 2016 | FAIL | 18.67 | 1.00 | 1370 | 321131 |

全部输出的组成：每源 WHEP、RTMP、FLV、RTSP 各 1，HLS ¼，GB UDP ½，GB TCP 主动 ¼，GB TCP 被动 ¼；WHEP 用 `stereo=1` offer。

**解读**
- 6 种非 HLS、非 WHEP 输出的边界相同，差别小于 32 源的搜索步长。每源 2 个观看者时，每源约 37 Mcycles/s 的固定成本中，Opus→AAC 占 23。
- HLS 观看者最便宜，所以多承载 64 源。
- WHEP 每观看者 5.2 M，所以少 96 源。
- Chrome offer 每源再加约 20 M，所以比直通再少 128 源。
- 带宽（最大持续点）：
  - 非 HLS、非 WHEP 输出：输入约 1.0 Gbit/s，输出约 2.0 Gbit/s；
  - 全部输出：输入约 0.74 Gbit/s，输出约 3.9 Gbit/s。

**少源多观看**
- b2 形态：4 个 WHIP 源配 1000 个 TCP 观看者只用 1.7–1.85 Gcycles/s，400 个 WHEP 观看者用 2.2 G。
- 按每观看者成本推算，8 核可承载约 1.2 万个 TCP 类或约 3500 个 WHEP 观看者。
- 这一推算未实测：单机环回下客户端会先成为瓶颈（1000 TCP 观看者已用 1.6–2.0 客户端核）。

## 10. 每源、每观看者成本

汇总（Mcycles/s；8 核满载约 18 600）：

| 成本项 | 值 | 来源 |
| --- | --- | --- |
| WHIP 源（含 Opus 解码 + fdk AAC 编码，无观看者） | 35.6 | w-none c2−c1 |
| 其中 Opus 解码 | 7.4 | p-flv-c2：0.475 G / 64 |
| 其中 fdk AAC 编码 | 16.0 | p-flv-c2：1.027 G / 64 |
| WHIP 源去掉转码后的接入成本（SRTP 解密、RTP 解包、H.264 解析、调度） | 约 12 | 差值 |
| 不需要转码的 RTMP/RTSP/GB 源 | 12–19 | 第 8 节 |
| AAC→Opus egress（每源一个，所有非直通 WHEP 观众共享） | 约 19.7 | p-whepchrome-c2 − p-whep-c2：libopus 编码 1.146 G 加 AAC 解码约 0.11 G，64 源 |
| WHEP 观看者 | 5.2 | 其中 SRTP 约 0.75，`sendto`（含环回对端接收）约 3.0 |
| GB UDP 观看者 | 2.4 | |
| RTMP / FLV / RTSP / GB TCP 观看者 | 1.3–1.5 | |
| HLS 观看者 | 0.4 | |

## 11. 实际转码器数量与共享

使用 [transcoders.bt](transcoders.bt) 对 `audio_transcoder::startup`、析构以及 `whep_audio_egress` 构造/析构挂 uprobe。键为线程、输入编码、输出编码和输出声道。

| 运行 | Opus→AAC 2ch | AAC→Opus egress | egress 声道 | 分布线程 |
| --- | --- | --- | --- | --- |
| w-all-a（1 源，24 观看者含 1 WHEP） | 1 | 0 | — | 1 |
| w-all-c（64 源，336 观看者含 64 WHEP） | 64 | 0 | — | 8 个 worker |
| w-allchrome-c（同上，WHEP 用 Chrome offer） | 64 | 64 | 1ch | 8 个 worker |
| lc-smoke3（4 源 + 1 个生命周期源，16 + 每轮 11 观看者） | 5 | 0 | — | 4 个 worker |

- 转码按源执行一次，与该源输出协议的种类和观看者数量无关；
- Chrome offer 的 WHEP 观众按源共享一个 egress；
- 视频帧由主流与 Opus 伴随流共享同一份 `media_frame`（profile 中没有视频复制或重复解析热点：H.264 解析 64 源仅 0.06 G，与输出种类无关）。

释放情况见第 16 节。

## 12. Worker 负载分布

8 个 worker 线程的 CPU（核）：

| 运行 | 最忙 | 最闲 | 差 |
| --- | --- | --- | --- |
| w-whep-c2 | 0.451 | 0.437 | 0.014 |
| w-all-c | 0.532 | 0.516 | 0.016 |
| w-whep-b2（4 源） | 0.344 | 0.298 | 0.046 |
| w-flv-b2（4 源） | 0.290 | 0.235 | 0.055 |

- 多源形态差距 ≤0.02 核。
- 少源多观看形态下，4 个源所在的 worker 各多约 0.05 核，即源侧接收与转码的成本；观看者已按会话数放置到全部 8 个 worker。
- 两种形态都不存在调度失衡。

## 13. CPU、内存与网络热点

`perf record -F 499 -g -C 0-7`，在测量窗口第 20 s 起采 15 s，按符号分类后换算为绝对 Gcycles/s（[prof_classify.py](prof_classify.py)）。分类合计与 `perf stat` 相差 <3%（例如 p-flv-c2 为 2.83 对 2.77）。

**多源形态（64 WHIP 源，128 观看者）**

| 类别 | WHEP 直通 | WHEP Chrome offer | FLV |
| --- | --- | --- | --- |
| fdk AAC 编码 | 1.006 | 1.032 | 1.027 |
| FFmpeg Opus 解码 | 0.473 | 0.518 | 0.475 |
| libopus 编码（egress） | 0.005 | 1.146 | 0.003 |
| AAC 解码（egress） | — | 约 0.11 | — |
| SRTP/加密 | 0.178 | 0.166 | 0.078 |
| H.264 解析 | 0.059 | 0.058 | 0.062 |
| 内存分配/复制（用户态） | 0.122 | 0.147 | 0.110 |
| asio 调度 | 0.072 | 0.072 | 0.124 |
| 内核：发送系统调用（含环回对端接收） | 0.431 | 0.399 | 0.201 |
| 　其中环回对端接收软中断 | 0.147 | 0.126 | 0.073 |
| 内核其他（调度、epoll、长尾） | 约 0.5 | 约 0.5 | 约 0.5 |
| 合计（不含 idle） | 2.94 | 4.22 | 2.70 |

**少源多观看形态**（4 WHIP 源，观看者数见运行名）

| 类别 | WHEP 400 | RTMP 1000 | FLV 1000 | RTSP 1000 | GB UDP 400 | GB TCP 200 | HLS 200 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 发送系统调用（含环回对端接收） | 1.191 | 0.903 | 0.935 | 0.918 | 0.714 | 0.168 | 0.039 |
| 　其中环回对端接收软中断 | 0.341 | 0.314 | 0.329 | 0.313 | 0.189 | 0.060 | 0.010 |
| SRTP | 0.300 | 0.006 | 0.005 | 0.006 | 0.005 | 0.005 | 0.005 |
| 用户态封装与分发（other user + asio + alloc） | 0.416 | 0.431 | 0.555 | 0.598 | 0.109 | 0.115 | 0.059 |
| 音频转码（4 源） | 0.094 | 0.093 | 0.090 | 0.092 | 0.091 | 0.092 | 0.097 |
| 合计（不含 idle） | 2.22 | 1.68 | 1.81 | 1.85 | 1.08 | 0.46 | 0.26 |

**热点排名**（按实际 CPU）
1. 每源 fdk AAC 编码，约 16 Mcycles/s。协议必需，前提是有 AAC 类消费者。
2. 每个非直通 WHEP 源的 libopus egress 编码，约 18 Mcycles/s。对 AAC 源必需；对 WHIP 源属于 Opus→AAC→Opus 的双重转码。
3. 逐包发送系统调用。WHEP/GB UDP 每包一次 `sendto`；TCP 类已批量。
4. 每源 Opus 解码，7.4 Mcycles/s。
5. SRTP：每 WHEP 观看者 0.75 Mcycles/s，加上每 WHIP 源的解密。

内存热点：
- HLS 每源分片缓存约 4.5–5.7 MiB；
- TCP 观看者发送队列每个约 0.25 MiB；
- 其他均 <0.2 MiB/会话。

**锁竞争**：`futex` 系统调用合计 ≤0.002 G，不是瓶颈。

**环回测量局限**
- 环回发送时，接收方的协议栈处理（`net_rx_action`）在发送方上下文同步执行。服务端 CPU 上因此记入了客户端的接收工作。
- 少源多观看形态中这部分约占服务端非空闲 cycles 的 15–18%，WHEP 400 观众时为 0.34 G。
- 上表已单列，但内核发送路径本身（skb、TCP 状态机）无法在单机上与真实网卡发送区分。
- 双机部署时 WHEP/TCP fanout 的每观看者成本约低 1/4–1/3；本报告的容量数字因此偏保守。

## 14. WHEP SDP 兼容性与直通条件

**代码规则**（`media/webrtc/whep.cpp:201`、`media/webrtc/webrtc_sdp.cpp:442,888`）
- WHIP 源和 WHEP Pull 源的 Opus 轨道一律按 2 声道登记，WHIP answer 不带 fmtp。
- WHEP 观众先按主流（AAC）协商；只有结果为 AAC 时，才对 Opus 伴随流重新协商一次。
- 伴随流协商要求观众 offer 同时满足：
  - 带 `stereo=1`；
  - `maxplaybackrate` 缺省或等于 48000；
  - `maxaveragebitrate` 缺省或等于 510000；
  - 媒体级 `a=maxptime` 缺省或等于 120。
- 满足时直通原始 Opus，否则走共享 egress：
  - offer 带 `stereo=1` 时 egress 输出 2ch 128k；
  - 否则输出 1ch 64k。

**与 RFC 7587 的对照**
- `stereo`：表示接收端偏好。单一目的地的发送端在 `stereo=0` 时 MUST NOT 发立体声；向多个目的地分发同一编码的网关 SHOULD NOT。服务端属于后者，按 SHOULD 处理是保守但正确的。
- `maxaveragebitrate`：发送端 MUST NOT 超过。源码率未知，只能要求“无限制”。
- `maxplaybackrate`：网关 SHOULD NOT 超过。只接受 48000，同理。
- `maxptime`：缺省为 120，源为 20 ms，原则上可放宽到 ≥20。目前只接受 120，属于保守处理，没有浏览器发此行。
- 结论：检查与 RFC 一致，本轮不放宽。[protocols.md](../../protocols.md) 原先只写了 `stereo=1`，比代码宽松，已按代码补全。

**fmtp 边界实测**（[sdp_boundary.py](sdp_boundary.py)，WHIP 立体声源，每个 offer 一个观众）

全部 12 个变体与预期一致，观众都收到媒体：

| offer | 结果 |
| --- | --- |
| `minptime=10;useinbandfec=1;stereo=1`（whep_fanout 默认） | 直通 |
| `minptime=10;useinbandfec=1`（Chrome 默认） | egress 1ch |
| `maxplaybackrate=48000;stereo=1;useinbandfec=1`（Firefox 默认） | 直通 |
| `stereo=0` | egress 1ch |
| `stereo=1;maxplaybackrate=24000` | egress 2ch |
| `stereo=1;maxplaybackrate=96000`（越界） | egress 2ch |
| `stereo=1;maxaveragebitrate=510000` | 直通 |
| `stereo=1;maxaveragebitrate=128000` | egress 2ch |
| `stereo=1;maxaveragebitrate=abc`（非法） | egress 2ch |
| `stereo=1;cbr=1`（接收偏好，不影响直通） | 直通 |
| `stereo=1` 加 `a=maxptime:120` | 直通 |
| `stereo=1` 加 `a=maxptime:60` | egress 2ch |

1 声道轨道（不要求 `stereo=1`）的分支无法从外部触发：WHIP 与 WHEP Pull 源都按 2 声道登记。

**真实浏览器**（[browser_webrtc.py](browser_webrtc.py)，Playwright 1.55.0）

| 浏览器 | 默认 Opus fmtp | 对 WHIP 源 | 对 RTMP AAC 源 |
| --- | --- | --- | --- |
| Chromium 140 / Google Chrome 156 | `minptime=10;useinbandfec=1` | egress 1ch 64k（answer `sprop-stereo=0`）；解码音频 39–49 万样本、视频 204–253 帧 1280 宽 | egress 1ch |
| Firefox 141（OpenH264 2.6.0） | `maxplaybackrate=48000;stereo=1;useinbandfec=1` | 直通（服务端未创建 egress） | egress 2ch 128k |

- 两者都不发 `a=maxptime`。
- 命中率：
  - Firefox 100% 直通；
  - Chrome 0%，因为它从不发 `stereo=1`。
  - 压测客户端 `whep_fanout` 的默认 offer 带 `stereo=1`，等价于 Firefox。上一轮 B 混合的直通收益因此只代表 Firefox 类观众。
- Chrome 回退的 CPU 成本：每个有 Chrome 观众的源多一个 egress，约 19.7 Mcycles/s，与观众数无关；观众本身的发送成本与直通相同。
- Firefox 客户端解码 BLOCKED。无头 Firefox 内容进程在 WebRTC 接收时 SIGSEGV，关闭 GMP/content/RDD/socket 沙箱后依旧。因此只验证了 offer 与服务端的直通选择，未验证 Firefox 实际解码。

**Chrome 作为 WHIP 推流端**（附带发现，均为既有行为）
- 默认 `degradationPreference` 下，约 8 s 后 Chrome 降分辨率。服务端记录 `whip input video config changed` 并结束会话，这是 WHIP 输入固定编码参数的既有设计。设为 `maintain-resolution` 后稳定。
- 服务端不向推流端发 PLI。晚加入的 WHEP 观众要等下一个关键帧，这是历史已知问题。
- Chrome 推的 Opus 是单声道内容：TOC 536 包中 stereo 位 0 次，fullband 20 ms，约 22–24 kbit/s。但服务端按 2 声道登记，于是：
  - 编码为立体声 AAC 128k；
  - Chrome 观众因 offer 不带 `stereo=1` 必走 egress。
- 若按实际声道登记为 1ch，`opus_passthrough_compatible` 对 1ch 轨道不要求 `stereo=1`，Chrome 观众即可直通。这需要在运行时从 TOC 判断声道，不能用 offer 中的 `sprop-stereo`：FFmpeg 的 WHIP 推流不发 fmtp，而且要求 2 声道输入。见第 17 节。

## 15. 音画同步、时间戳与解码

| 范围 | 结果 |
| --- | --- |
| 矩阵 20 个媒体场景 | 全部 PASS；解码告警 0；视频 PTS 无回退、无缺口；音频无缺口/重叠 |
| WHIP 源，各输出 | 偏差 −4.0…−5.3 ms（RTMP/RTSP/FLV/HLS/WHEP 一致） |
| AAC 源 | 0.5–9.7 ms |
| RTSP Pull | 11.5–12.3 ms |
| H.265 源（RTMP/RTSP） | 13–22 ms，四种输出一致，来自源侧时间线，非输出路径问题 |
| B 帧源 WHEP | 3.7 ms（工具修复后） |
| H.265 源→WHEP（补测） | RTMP 26.7 ms，RTSP 9.7 ms；解码告警 0，PTS 无回退。RTMP 一项高于其他输出的 13 ms，但仍在单帧内 |
| G.711→WHEP | 用修复后的工具离线重放矩阵 dump：RTMP PCMA/PCMU 11.0 ms，RTSP PCMA 8.5 ms，各 19 个标记。RTSP PCMA 录像有 1 处 51.6 ms 音频缺口，但 dump 中音频 RTP 序号连续、时间戳增量只有 168/176（源帧 1024/48000 s 重采样到 8 kHz），属回放录制伪像，原始 dump 已保留 |

所有偏差都在单帧（33 ms）以内，且同一源的各输出一致。

- W 扫描的 a 形态在每条输出上运行了 A/V 探测与解码探测（RTMP/RTSP/FLV/HLS/WHEP），全部通过：解码告警 0，视频 PTS 无回退、无缺口。
- 压测素材 `perf_fixture_whip.mkv` 没有同步标记，所以这些运行只验证时间线与解码，偏差以矩阵中的 WHIP 场景为准。
- 局限：GB 输出（UDP/TCP）在矩阵和压测中都只验证到收包推进，没有解码或音画同步探测。这是现有工具的覆盖缺口，本轮未补。

## 16. Sanitizer、生命周期与资源清理

**WHIP 源生命周期（Release，`--source-churn-input whip`）**
- 基础负载：8 个常驻 WHIP 源，带 16 WHEP + 16 FLV 观看者。
- 每 60 s 一轮生命周期源，共 3 轮，依次以 SIGTERM（WHIP DELETE）、SIGKILL、SIGTERM 结束发布。
- 每轮生命周期源带 12 个观看者，覆盖 8 种协议：WHEP 2、FLV 2、RTSP 2、RTMP 2，GB UDP、GB TCP 主动、GB TCP 被动、HLS 各 1。
- 前一半协议组在停止发布前退出，其余组须在源结束后 15 s 内退出。
- lc-full-b 交换了两半的顺序，并使用 Chrome offer，以覆盖 egress。

| 项 | lc-full-a | lc-full-b |
| --- | --- | --- |
| 结果 | PASS，3/3 轮 | PASS，3/3 轮 |
| 发布就绪 / 全部观看者就绪 | 0.21 s / 4.03 s | 0.21 s / 4.04 s |
| 源消失：DELETE / SIGKILL | 1.5–2.2 ms / 20.1 s | 1.4 ms / 20.1 s |
| 剩余观看者退出（自源消失） | 0.3–0.5 ms，无滞留 | 0.3–0.5 ms，无滞留 |
| 每轮观看者加入前 / 退出后 FD | 73 / 72，三轮相同 | 73 / 72，三轮相同 |
| Opus→AAC 转码器启动 / 释放 | 11 / 11 | 11 / 11 |
| AAC→Opus egress 转码器启动 / 释放 | 0 / 0 | 11 / 11 |
| 清理后 FD / 遗留 receiver | 32（基线 40）/ 0 | 32 / 0 |
| 清理后 PSS（基线 35.8 MiB） | 39.3 MiB | 47.5 MiB |

- SIGKILL 的 20.1 s 由 ICE 活动超时回收：超时 30 s，只由 STUN 刷新，客户端每 10 s 一次 STUN。
- egress 对象的析构被内联进 `shared_ptr` 的 dispose，uprobe 无法观测，以其所属转码器的析构为准。
- 清理后 PSS 的增量与上一轮同口径，为 jemalloc 保留的脏页。FD 与转码器计数说明没有泄漏。

**Sanitizer**（`3b6c0bd` 的 ASan/TSan 构建，4 worker）

| 构建 | 矩阵子集 | WHIP 生命周期 | 报告 |
| --- | --- | --- | --- |
| ASan（`detect_leaks=1`） | 11/11 PASS | PASS（4 源 + 2 轮，SIGTERM/SIGKILL，Chrome offer，每轮 12 个 8 协议观看者） | 15 个日志均只有协程 `swapcontext` 固定提示 |
| TSan | 11/11 PASS | PASS（同上） | 无报告；服务端输出中无 ThreadSanitizer 字样 |

- 矩阵子集：rtmp-h264-aac、rtmp-h264-bframes-aac、rtsp-tcp-h264-opus、whip-h264-opus、rtsp-pull、whep-pull、gb-udp、multi-source-cross-worker、fault-source-stop、fault-invalid-token-whep、fault-replayed-token-whep。
- 故障注入类以 `PASS UNCONFIRMED exit=1` 结束，属正常。
- 历史偶发的 ASan SRTP `replay_fail`：本轮未复现。全部 ASan/TSan 服务端日志中没有 `replay_fail`，也没有 SRTP protect 失败。

## 17. 优化方案

本轮未修改生产代码，没有满足“修改代码的条件”且无需产品决策的热点。

**按需 Opus→AAC（只测量，未实施）**
- 无 AAC 消费者时，每 WHIP 源可省约 23.4 Mcycles/s（Opus 解码 7.4 + fdk 编码 16.0），约为每源成本的 2/3。
- 适用场景：
  - 纯 WHEP 直通（Firefox 类 offer）：64 源 128 观看者从 2.94 G 降到约 1.45 G，同形态 WHIP→WHEP 容量约可翻倍；
  - 无观看者的空闲 WHIP 源：每源从 35.6 降到约 12。
- 不适用场景：
  - 源上只要有任一 RTMP/FLV/RTSP/HLS/GB 观众，就必须转码；
  - 有 Chrome 默认 offer 的观众时同样必须转码：egress 从 AAC 主流解码，没有 AAC 就无法服务 Chrome。
  - 按目前浏览器份额，WebRTC 观众多数属于后者。
- 代价：
  - 需要按主流音频消费者数量启停转码器，即新增“空闲/运行”状态和启停时机；
  - 首个 AAC 观众的音频要等转码器启动，再加编码器延迟补偿（fdk 2048 样本，约 43 ms 内）；
  - 视频仍需等关键帧，所以首帧延迟主要由 GOP 决定；
  - 重启转码器要处理时间线连续性，可复用现有 `initial_padding` 补偿，但需要新增测试。
- 判断：收益集中在“只用 Firefox 或经过 offer 改写的 WebRTC 观众”和“大量空闲 WHIP 源”两类部署。没有业务比例作依据，不建议直接实施，列为决策项。

**WHIP 按实际 Opus 声道登记（未实施）**
- 收益：
  - Chrome→Chrome 观看免去 egress，每源约 19.7 Mcycles/s；
  - 少一次有损重编码；
  - 单声道源的 AAC 编码可降为 1ch，fdk 成本约减半，但这会改变现有 AAC 输出声道，需另行确认。
- 代价：首批 Opus 包到达前，轨道参数不能确定，需要延迟登记，或在运行时改变声道。
- 判断：属于行为变更，需决策。

**拒绝的方向**
- UDP `sendmmsg`/GSO 批量发送：
  - WHEP 与 GB UDP 每包一次 `sendto`，b2 形态中占 0.7–1.2 G；
  - 其中约 30% 是环回对端接收，单机上无法得到可信的收益数据；
  - 批量只省系统调用进出，逐包协议栈处理照旧；
  - 预计收益 <10%，本轮不做。
- 上一轮已拒绝、本轮没有新证据的方向不再重复：libopus complexity 7、降码率、GOP/history/reader、通用媒体图、线程池扩容、sysctl 调大缓冲区、端口扫描。

## 18. 历史遗留与本轮新问题

| 问题 | 性质 | 处理 |
| --- | --- | --- |
| WHEP B 帧回放录制失败 | 工具缺陷（新发现，上一轮工具未覆盖） | 已修复，复跑 PASS |
| 矩阵 H.265 场景不含 WHEP 输出 | 回归覆盖缺口 | 本轮用 perf_run 补测 PASS；是否把 WHEP 加入矩阵的 H.265 场景，待决定 |
| G.711→WHEP 音频未被 A/V 探测 | 工具缺陷（新发现） | 已修复，见第 15 节 |
| `whep_fanout` 默认 offer 带 `stereo=1`，不代表 Chrome | 测试口径 | 新增 `--whep-opus-fmtp`，Chrome offer 的运行单列 |
| `protocols.md` 直通条件比代码宽松 | 文档 | 已按代码补全 |
| 上一轮报告“WHIP/WHEP Pull 源向 WHEP 观众直通”的收益 | 口径勘误 | 只适用于带 `stereo=1` 的 offer（Firefox、压测客户端），不适用于 Chrome 默认 offer |
| Chrome WHIP 降分辨率后会话被结束 | 既有设计 | 未改；记录 |
| 服务端不向 WHIP 推流端发 PLI | 历史已知 | 未改；记录 |
| Chrome WHIP 单声道按 2ch 登记 | 既有行为 | 决策项 |
| i-rtmp-aac-none 单次 0.42 G，指令数高 5 倍（同素材 RTSP 0.20 G） | 未复现 | 复跑 i2-rtmp-aac-none 为 0.24 G，与其他 AAC 输入一致；首次原因未查明 |
| Firefox 无头接收崩溃 | 环境/浏览器 | BLOCKED |
| ASan SRTP `replay_fail` | 历史偶发 | 本轮 ASan 矩阵子集与 WHIP 生命周期未复现 |
| RTSP PCMA→WHEP 录像 1 处音频缺口 | 回放工具伪像 | RTP 连续，dump 已保留 |

## 19. 与上一轮 Mix A/B/C 的关系

- 二进制相同（`3b6c0bd` 构建，SHA256 一致），环境、素材、CPU 分配、预热/测量窗口与 PASS 判定相同；cycles 数据可以直接比较。
- 不可直接比较的部分：
  - 本轮按单一输出协议或固定比例测量，不是 A/B/C 的混合比例，“倍率”不能对应；
  - 上一轮 B 的 Opus 直通收益只对带 `stereo=1` 的 WHEP 观众成立。B 中 154 个直通在 Chrome 观众下会变成 154 个 egress，约多 3.0 G，B 的容量约回到原始的 1.2× 附近；
  - 上一轮矩阵工具没有 WHEP A/V 探测，本轮矩阵包含该探测，且修复了 B 帧和 G.711 两处回放缺口；
  - 上一轮 B 的数据也在缩小后的命名空间临时端口范围下测得，与本轮一致；A、C 早于该设置。

## 20. 结论：是否继续优化

**直接回答验收问题**
- **WHIP→各输出能承载多少路**：见第 9 节，每源 2 观看者时：
  - RTMP/FLV/RTSP/GB UDP/GB TCP 为 480 源（留余量 416）；
  - HLS 为 544（480）；
  - WHEP 直通为 384（352），Chrome offer 为 256（224）；
  - 全部输出同时为 352 源 / 1848 观看者（288 / 1512）。
- **CPU 消耗**：
  - WHIP 源固定约 35.6 Mcycles/s，其中转码 23.4；
  - 观看者 0.4（HLS）到 5.2（WHEP）Mcycles/s；
  - Chrome WHEP 每源再加 20。
- **共享的计算**：
  - Opus 解码与 AAC 编码按源一次；
  - 视频帧在主流与伴随流间共享同一缓冲；
  - 各协议封装按源或 worker 生成后分发给观看者；
  - AAC→Opus egress 按源共享。
- **无法避免的转码**：
  - AAC/G.711 类输出需要 AAC，所以 Opus 源要转 AAC；
  - AAC 源给 WHEP 需要转 Opus。
- **浪费**：
  - 纯 WHEP 直通或无人观看时，WHIP 源仍做 Opus→AAC；
  - Chrome 观众观看 WHIP 源时是 Opus→AAC→Opus 双重转码，而且源本身常为单声道。
  - 两者都需要产品决策（第 17 节）。
- **完整矩阵**：
  - 受支持的组合全部正确；
  - CPU 最高的是带 Chrome WHEP 观众的 WHIP 源（每源约 63 M）；
  - 内存最高的是 HLS（每源约 5.7 MiB）；
  - 网络主导的是少源多观看的 WHEP/GB UDP/TCP fanout，发送系统调用占 40–55%；
  - 音频转码主导的是 WHIP/WHEP Pull 输入，以及带 WHEP 观众的 AAC 源。
- **多源少观看与少源多观看的差异**：多源由每源转码决定；少源多观看由每观看者的内核发送决定，其中约 1/3 是环回测量误差。

**架构判断**
- 现有转码与分发结构足够精简：没有重复转码器，没有重复封装，worker 均衡（差 ≤0.02 核），锁竞争可忽略。
- Opus 直通在 Firefox 下生效，在 Chrome 默认 offer 下不生效。
- 按需转码：有收益的场景明确，但依赖部署中的观众构成，见决策项。
- 应停止研究的方向：
  - worker 放置调整；
  - TCP 发送合并（已批量）；
  - UDP `sendmmsg`/GSO：单机环回下无法可靠评估，预计 <10%；
  - libopus/fdk 参数降档：违反质量约束。

| 路径 | 结论 |
| --- | --- |
| WHIP→RTMP/FLV/RTSP/HLS/GB UDP/GB TCP | GO |
| WHIP→WHEP（Firefox 类直通） | GO |
| WHIP→WHEP（Chrome 默认 offer） | OPTIMIZE 候选（按实际声道登记），需决策 |
| WHIP 源的 Opus→AAC（无 AAC 消费者时） | OPTIMIZE 候选（按需转码），需决策 |
| WHIP→全部输出 | GO |
| 其他输入×编码×输出 | GO |
| 少源多观看 fanout 的网络发送 | GO（单机下无法进一步量化；如需确认，应做双机测量） |
| Firefox 端到端解码 | BLOCKED（浏览器环境） |

## 复现

运行时目录 `<TEST_HOST>:/var/tmp/ms-perf-891aec1`，即 `$T`。

| 内容 | 位置 |
| --- | --- |
| 运行时 | `$T/rt-q`（`run.sh` 设置 `MEDIA_SERVER_BENCH_HEAD`） |
| 每次运行 | `$T/runs2/<name>/result.json.gz` 与 `<name>.env.json` |
| Profile 与分类 | `$T/runs2/prof/<name>/` |
| bpftrace 输出 | `$T/runs2/trace/` |
| 浏览器结果 | `$T/runs2/browser8/result.json` |
| 作业脚本 | `$T/jobs-done/` 与 `$T/jobs/` |

脚本：

| 脚本 | 用途 |
| --- | --- |
| [env_check.sh](env_check.sh) | 环境检查 |
| [run_perf.sh](run_perf.sh) | 单次运行包装 |
| [profile_run.sh](profile_run.sh) | profile |
| [prof_classify.py](prof_classify.py) | profile 分类 |
| [costs.py](costs.py) | 成本表 |
| [matrix_table.py](matrix_table.py) | 矩阵表 |
| [transcoders.bt](transcoders.bt) | 转码器计数 |
| [browser_webrtc.py](browser_webrtc.py) | 浏览器测试 |
| [sdp_boundary.py](sdp_boundary.py) | fmtp 边界 |
| [lib.sh](lib.sh)、[sum.py](sum.py) | 作业公共定义与单次结果摘要 |
| [jobs/](jobs/) | 本轮按顺序执行的作业：q01 矩阵、q05 WHIP 摸底、q06/q10 生命周期、q07 输入组合、q08 profile、q09/q13 容量、q11 Sanitizer、q12 SDP 边界与回放、q14 H.265→WHEP 补测 |

## 文件

| 文件 | 改动 |
| --- | --- |
| `bench/perf_run.py` | 新增 `--source-churn-input whip`、`--source-churn-viewers`、`--whep-opus-fmtp` |
| `bench/whep_dump_av.py` | 适配 B 帧重排；支持 G.711 音频 |
| `bench/whep_fanout.cpp`、`bench/clients/webrtc_client.{h,cpp}` | 新增 `--opus-fmtp` |
| `docs/protocols.md` | 直通条件按代码补全 |
| `docs/performance.md` | 记录新增选项 |
| 本目录 | 报告与脚本 |

## 附录 A：功能矩阵逐项

后一份 summary 覆盖前一份的同名场景（`mx-rel-bframes` 为修复回放工具后复跑的 3 项）。A/V 列为各输出的音画偏差 p50（ms）；没有音轨的输出（HLS 的 Opus/G.711）记为 0.0。

| scenario | run | result | kind | groups ready/requested | A/V probe (offset p50 ms) | decode probe | FD base/after | receivers left |
|---|---|---|---|---|---|---|---|---|
| rtmp-h264-aac | mx-rel-bframes | PASS | media | rtmp-0:2/2 rtsp-0:2/2 flv-0:2/2 hls-0:1/1 whep-0:2/2 gb_udp-0:1/1 gb_tcp_active-0:1/1 gb_tcp_passive-0:1/1 | rtmp:ok(0.7) rtsp:ok(1.0) flv:ok(0.7) hls:ok(1.0) whep:ok(0.7) | rtmp:ok rtsp:ok flv:ok hls:ok | 21/20 | 0 |
| rtmp-h264-bframes-aac | mx-rel-bframes | PASS | media | rtmp-0:2/2 rtsp-0:2/2 flv-0:2/2 hls-0:1/1 whep-0:2/2 gb_udp-0:1/1 gb_tcp_active-0:1/1 gb_tcp_passive-0:1/1 | rtmp:ok(1.3) rtsp:ok(1.0) flv:ok(1.3) hls:ok(1.7) whep:ok(3.7) | rtmp:ok rtsp:ok flv:ok hls:ok | 21/20 | 0 |
| rtmp-h265-aac | mx-rel | PASS | media | rtmp-0:2/2 rtsp-0:2/2 flv-0:2/2 hls-0:1/1 gb_udp-0:1/1 gb_tcp_active-0:1/1 gb_tcp_passive-0:1/1 | rtmp:ok(13.3) rtsp:ok(13.0) flv:ok(13.3) hls:ok(13.7) | rtmp:ok rtsp:ok flv:ok hls:ok | 21/20 | 0 |
| rtmp-h264-pcma | mx-rel | PASS | media | rtmp-0:2/2 rtsp-0:2/2 flv-0:2/2 hls-0:1/1 whep-0:2/2 gb_udp-0:1/1 gb_tcp_active-0:1/1 gb_tcp_passive-0:1/1 | rtmp:ok(5.7) rtsp:ok(5.8) flv:ok(5.7) hls:ok(0.0) whep:ok(0.0) | rtmp:ok rtsp:ok flv:ok hls:ok | 21/20 | 0 |
| rtmp-h264-pcmu | mx-rel | PASS | media | rtmp-0:2/2 rtsp-0:2/2 flv-0:2/2 hls-0:1/1 whep-0:2/2 gb_udp-0:1/1 gb_tcp_active-0:1/1 gb_tcp_passive-0:1/1 | rtmp:ok(5.7) rtsp:ok(5.7) flv:ok(5.7) hls:ok(0.0) whep:ok(0.0) | rtmp:ok rtsp:ok flv:ok hls:ok | 21/20 | 0 |
| rtmp-h264-opus | mx-rel | PASS | media | rtmp-0:2/2 rtsp-0:2/2 flv-0:2/2 hls-0:1/1 whep-0:2/2 | rtmp:ok(2.7) rtsp:ok(2.7) flv:ok(2.7) hls:ok(0.0) whep:ok(2.7) | rtmp:ok rtsp:ok flv:ok hls:ok | 21/20 | 0 |
| rtsp-tcp-h264-aac | mx-rel | PASS | media | rtmp-0:2/2 rtsp-0:2/2 flv-0:2/2 hls-0:1/1 whep-0:2/2 gb_udp-0:1/1 gb_tcp_active-0:1/1 gb_tcp_passive-0:1/1 | rtmp:ok(6.0) rtsp:ok(5.2) flv:ok(6.0) hls:ok(9.7) whep:ok(9.7) | rtmp:ok rtsp:ok flv:ok hls:ok | 21/20 | 0 |
| rtsp-udp-h264-aac | mx-rel | PASS | media | rtmp-0:2/2 rtsp-0:2/2 flv-0:2/2 hls-0:1/1 whep-0:2/2 gb_udp-0:1/1 gb_tcp_active-0:1/1 gb_tcp_passive-0:1/1 | rtmp:ok(6.0) rtsp:ok(5.2) flv:ok(6.0) hls:ok(9.7) whep:ok(9.7) | rtmp:ok rtsp:ok flv:ok hls:ok | 25/20 | 0 |
| rtsp-tcp-h265-aac | mx-rel | PASS | media | rtmp-0:2/2 rtsp-0:2/2 flv-0:2/2 hls-0:1/1 gb_udp-0:1/1 gb_tcp_active-0:1/1 gb_tcp_passive-0:1/1 | rtmp:ok(18.0) rtsp:ok(19.2) flv:ok(18.0) hls:ok(21.7) | rtmp:ok rtsp:ok flv:ok hls:ok | 21/20 | 0 |
| rtsp-tcp-h264-opus | mx-rel | PASS | media | rtmp-0:2/2 rtsp-0:2/2 flv-0:2/2 hls-0:1/1 whep-0:2/2 | rtmp:ok(2.7) rtsp:ok(2.6) flv:ok(2.7) hls:ok(0.0) whep:ok(2.7) | rtmp:ok rtsp:ok flv:ok hls:ok | 21/20 | 0 |
| rtsp-tcp-h264-pcma | mx-rel | PASS | media | rtmp-0:2/2 rtsp-0:2/2 flv-0:2/2 hls-0:1/1 whep-0:2/2 gb_udp-0:1/1 gb_tcp_active-0:1/1 gb_tcp_passive-0:1/1 | rtmp:ok(4.9) rtsp:ok(4.9) flv:ok(4.9) hls:ok(0.0) whep:ok(0.0) | rtmp:ok rtsp:ok flv:ok hls:ok | 21/20 | 0 |
| whip-h264-opus | mx-rel-bframes | PASS | media | rtmp-0:2/2 rtsp-0:2/2 flv-0:2/2 hls-0:1/1 whep-0:2/2 gb_udp-0:1/1 gb_tcp_active-0:1/1 gb_tcp_passive-0:1/1 | rtmp:ok(-4.0) rtsp:ok(-5.3) flv:ok(-4.0) hls:ok(-4.7) whep:ok(-4.3) | rtmp:ok rtsp:ok flv:ok hls:ok | 21/20 | 0 |
| rtmp-pull | mx-rel | PASS | media | rtmp-0:2/2 rtsp-0:2/2 flv-0:2/2 hls-0:1/1 whep-0:2/2 gb_udp-0:1/1 gb_tcp_active-0:1/1 gb_tcp_passive-0:1/1 | rtmp:ok(1.5) rtsp:ok(1.3) flv:ok(1.5) hls:ok(1.3) whep:ok(4.0) | rtmp:ok rtsp:ok flv:ok hls:ok | 21/20 | 0 |
| rtsp-pull | mx-rel | PASS | media | rtmp-0:2/2 rtsp-0:2/2 flv-0:2/2 hls-0:1/1 whep-0:2/2 gb_udp-0:1/1 gb_tcp_active-0:1/1 gb_tcp_passive-0:1/1 | rtmp:ok(11.5) rtsp:ok(11.8) flv:ok(11.5) hls:ok(11.8) whep:ok(12.3) | rtmp:ok rtsp:ok flv:ok hls:ok | 21/20 | 0 |
| whep-pull | mx-rel | PASS | media | rtmp-0:2/2 rtsp-0:2/2 flv-0:2/2 hls-0:1/1 whep-0:2/2 gb_udp-0:1/1 gb_tcp_active-0:1/1 gb_tcp_passive-0:1/1 | rtmp:ok(0.5) rtsp:ok(1.0) flv:ok(0.5) hls:ok(0.0) whep:ok(0.7) | rtmp:ok rtsp:ok flv:ok hls:ok | 21/20 | 0 |
| gb-udp | mx-rel | PASS | media | rtmp-0:2/2 rtsp-0:2/2 flv-0:2/2 hls-0:1/1 whep-0:2/2 gb_udp-0:1/1 gb_tcp_active-0:1/1 gb_tcp_passive-0:1/1 | rtmp:ok(2.0) rtsp:ok(2.0) flv:ok(2.0) hls:ok(2.0) whep:ok(1.0) | rtmp:ok rtsp:ok flv:ok hls:ok | 22/20 | 0 |
| gb-tcp-active | mx-rel | PASS | media | rtmp-0:2/2 rtsp-0:2/2 flv-0:2/2 hls-0:1/1 whep-0:2/2 gb_udp-0:1/1 gb_tcp_active-0:1/1 gb_tcp_passive-0:1/1 | rtmp:ok(2.0) rtsp:ok(2.0) flv:ok(2.0) hls:ok(2.0) whep:ok(1.0) | rtmp:ok rtsp:ok flv:ok hls:ok | 21/20 | 0 |
| gb-tcp-passive | mx-rel | PASS | media | rtmp-0:2/2 rtsp-0:2/2 flv-0:2/2 hls-0:1/1 whep-0:2/2 gb_udp-0:1/1 gb_tcp_active-0:1/1 gb_tcp_passive-0:1/1 | rtmp:ok(2.0) rtsp:ok(2.0) flv:ok(2.0) hls:ok(2.0) whep:ok(1.0) | rtmp:ok rtsp:ok flv:ok hls:ok | 21/20 | 0 |
| multi-source-cross-worker | mx-rel | PASS | media | rtmp-0:20/20 rtsp-0:20/20 flv-0:20/20 hls-0:4/4 whep-0:12/12 gb_udp-0:8/8 gb_tcp_active-0:4/4 gb_tcp_passive-0:4/4 | rtmp:ok(0.0) rtsp:ok(0.2) flv:ok(0.0) hls:ok(0.2) whep:ok(3.3) | rtmp:ok rtsp:ok flv:ok hls:ok | 24/20 | 0 |
| slow-readers | mx-rel | PASS | media | rtmp-0:2/2 rtsp-0:2/2 flv-0:2/2 | rtmp:ok(2.5) rtsp:ok(2.8) flv:ok(2.5) | rtmp:ok rtsp:ok flv:ok | 21/20 | 0 |
| fault-source-stop | mx-rel | PASS | expected SOURCE_ENDED | rtmp-0:2/2 rtsp-0:2/2 flv-0:2/2 whep-0:2/2 gb_udp-0:1/1 | rtmp:ok(2.5) rtsp:ok(3.0) flv:ok(2.5) whep:ok(0.7) | rtmp:ok rtsp:ok flv:ok | 21/20 | 0 |
| fault-invalid-token-rtmp | mx-rel | PASS | expected AUTHORIZATION | rtmp-0:0/1 | — | — | 21/20 | 0 |
| fault-invalid-token-rtsp | mx-rel | PASS | expected AUTHORIZATION | rtsp-0:0/1 | — | — | 21/20 | 0 |
| fault-invalid-token-flv | mx-rel | PASS | expected AUTHORIZATION | flv-0:0/1 | — | — | 21/20 | 0 |
| fault-invalid-token-hls | mx-rel | PASS | expected AUTHORIZATION | hls-0:0/1 | — | — | 21/20 | 0 |
| fault-invalid-token-whep | mx-rel | PASS | expected AUTHORIZATION | whep-0:None/None | — | — | 21/20 | 0 |
| fault-replayed-token-rtmp | mx-rel | PASS | expected AUTHORIZATION | rtmp-0:0/1 | — | — | 21/20 | 0 |
| fault-replayed-token-rtsp | mx-rel | PASS | expected AUTHORIZATION | rtsp-0:0/1 | — | — | 21/20 | 0 |
| fault-replayed-token-flv | mx-rel | PASS | expected AUTHORIZATION | flv-0:0/1 | — | — | 21/20 | 0 |
| fault-replayed-token-hls | mx-rel | PASS | expected AUTHORIZATION | hls-0:0/1 | — | — | 21/20 | 0 |
| fault-replayed-token-whep | mx-rel | PASS | expected AUTHORIZATION | whep-0:None/None | — | — | 21/20 | 0 |

## 附录 B：WHIP 路径摸底

`w-whepchrome2-*`、`w-allchrome2-c` 为 Chrome offer 的运行。最初的 `w-whepchrome-*`、`w-allchrome-c` 因包装脚本未转义 fmtp 中的分号，丢失了 `perf stat` 等参数，已剔除。

| run | status | src | viewers | Gcycles/s | Ginstr/s | IPC | busy cores | busiest core | softirq | worker spread | client cores | PSS MiB | FD | UDP drops | realtime min | probes | FD base/after | left |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| w-all-a | PASS | 1 | 24 | 0.21 | 0.17 | 0.78 | 0.08 | 0.05 | 0.004 | 0.06 | 0.10 | 37 | 69 | 0 | 1.000 | rtmp:ok rtsp:ok flv:ok hls:ok whep:ok decode-rtmp:ok decode-rtsp:ok decode-flv:ok decode-hls:ok | 33/32 | 0 |
| w-all-b | PASS | 4 | 675 | 1.79 | 1.94 | 1.09 | 1.91 | 0.27 | 0.328 | 0.06 | 1.52 | 155 | 761 | 0 | 1.000 | — | 36/32 | 0 |
| w-all-c | PASS | 64 | 336 | 3.63 | 5.07 | 1.40 | 4.27 | 0.54 | 0.321 | 0.02 | 1.66 | 226 | 464 | 0 | 1.000 | — | 96/32 | 0 |
| w-allchrome2-c | PASS | 64 | 336 | 4.83 | 7.33 | 1.52 | 5.65 | 0.71 | 0.281 | 0.00 | 1.66 | 283 | 465 | 0 | 0.999 | — | 96/32 | 0 |
| w-flv-a | PASS | 1 | 4 | 0.15 | 0.15 | 1.00 | 0.09 | 0.02 | 0.001 | 0.04 | 0.03 | 25 | 39 | 0 | 1.000 | flv:ok decode-flv:ok | 33/32 | 0 |
| w-flv-b1 | PASS | 4 | 500 | 1.07 | 1.12 | 1.05 | 1.18 | 0.16 | 0.203 | 0.05 | 0.97 | 104 | 536 | 0 | 1.000 | — | 36/32 | 0 |
| w-flv-b2 | PASS | 4 | 1000 | 1.81 | 1.99 | 1.10 | 2.07 | 0.28 | 0.403 | 0.05 | 2.00 | 235 | 1037 | 0 | 1.000 | — | 36/32 | 0 |
| w-flv-c1 | PASS | 32 | 64 | 1.55 | 2.08 | 1.35 | 1.54 | 0.21 | 0.041 | 0.01 | 0.49 | 70 | 128 | 0 | 1.000 | — | 64/32 | 0 |
| w-flv-c2 | PASS | 64 | 128 | 2.77 | 4.09 | 1.48 | 3.16 | 0.40 | 0.080 | 0.01 | 0.83 | 119 | 225 | 0 | 1.000 | — | 96/32 | 0 |
| w-gb_tcp_active-a | PASS | 1 | 4 | 0.09 | 0.08 | 0.85 | 0.10 | 0.07 | 0.001 | 0.04 | 0.02 | 25 | 37 | 0 | 1.000 | — | 33/32 | 0 |
| w-gb_tcp_active-b1 | PASS | 4 | 100 | 0.37 | 0.40 | 1.08 | 0.43 | 0.09 | 0.040 | 0.05 | 0.19 | 60 | 136 | 0 | 1.000 | — | 36/32 | 0 |
| w-gb_tcp_active-b2 | PASS | 4 | 200 | 0.51 | 0.56 | 1.09 | 0.58 | 0.11 | 0.091 | 0.05 | 0.31 | 88 | 236 | 0 | 1.000 | — | 36/32 | 0 |
| w-gb_tcp_active-c1 | PASS | 32 | 64 | 1.50 | 2.02 | 1.34 | 1.53 | 0.21 | 0.030 | 0.01 | 0.45 | 74 | 129 | 0 | 0.999 | — | 64/32 | 0 |
| w-gb_tcp_active-c2 | PASS | 64 | 128 | 2.76 | 3.99 | 1.45 | 3.12 | 0.40 | 0.100 | 0.01 | 0.77 | 126 | 225 | 0 | 1.000 | — | 96/32 | 0 |
| w-gb_tcp_passive-a | PASS | 1 | 4 | 0.09 | 0.08 | 0.86 | 0.08 | 0.03 | 0.001 | 0.04 | 0.02 | 25 | 37 | 0 | 1.000 | — | 33/32 | 0 |
| w-gb_tcp_passive-b1 | PASS | 4 | 100 | 0.38 | 0.41 | 1.07 | 0.50 | 0.11 | 0.065 | 0.05 | 0.19 | 60 | 137 | 0 | 1.000 | — | 36/32 | 0 |
| w-gb_tcp_passive-b2 | PASS | 4 | 200 | 0.52 | 0.56 | 1.09 | 0.61 | 0.10 | 0.085 | 0.05 | 0.31 | 88 | 237 | 0 | 1.000 | — | 36/32 | 0 |
| w-gb_tcp_passive-c1 | PASS | 32 | 64 | 1.52 | 2.03 | 1.33 | 1.66 | 0.22 | 0.058 | 0.01 | 0.45 | 74 | 129 | 0 | 1.000 | — | 64/32 | 0 |
| w-gb_tcp_passive-c2 | PASS | 64 | 128 | 2.69 | 3.98 | 1.48 | 3.03 | 0.39 | 0.083 | 0.01 | 0.78 | 126 | 225 | 0 | 1.000 | — | 96/32 | 0 |
| w-gb_udp-a | PASS | 1 | 4 | 0.09 | 0.09 | 0.92 | 0.09 | 0.05 | 0.003 | 0.04 | 0.02 | 25 | 41 | 0 | 1.000 | — | 33/32 | 0 |
| w-gb_udp-b1 | PASS | 4 | 200 | 0.64 | 0.80 | 1.25 | 0.69 | 0.10 | 0.118 | 0.05 | 0.35 | 35 | 436 | 0 | 1.000 | — | 36/32 | 0 |
| w-gb_udp-b2 | PASS | 4 | 400 | 1.12 | 1.36 | 1.22 | 1.45 | 0.22 | 0.288 | 0.05 | 0.67 | 36 | 836 | 0 | 1.000 | — | 36/32 | 0 |
| w-gb_udp-c1 | PASS | 32 | 64 | 1.54 | 2.11 | 1.37 | 1.66 | 0.21 | 0.043 | 0.01 | 0.47 | 67 | 192 | 0 | 1.000 | — | 64/32 | 0 |
| w-gb_udp-c2 | PASS | 64 | 128 | 2.79 | 4.15 | 1.49 | 3.21 | 0.41 | 0.118 | 0.01 | 0.83 | 112 | 353 | 0 | 0.999 | — | 96/32 | 0 |
| w-hls-a | PASS | 1 | 4 | 0.06 | 0.07 | 1.14 | 0.05 | 0.05 | 0.000 | 0.00 | 0.02 | 26 | 41 | 0 | — | hls:ok decode-hls:ok | 33/32 | 0 |
| w-hls-b1 | PASS | 4 | 100 | 0.24 | 0.28 | 1.19 | 0.20 | 0.04 | 0.005 | 0.05 | 0.17 | 46 | 136 | 0 | — | — | 36/32 | 0 |
| w-hls-b2 | PASS | 4 | 200 | 0.28 | 0.31 | 1.08 | 0.24 | 0.06 | 0.010 | 0.05 | 0.27 | 48 | 237 | 0 | — | — | 36/32 | 0 |
| w-hls-c1 | PASS | 32 | 64 | 1.32 | 1.99 | 1.51 | 1.34 | 0.19 | 0.004 | 0.02 | 0.42 | 219 | 129 | 0 | — | — | 64/32 | 0 |
| w-hls-c2 | PASS | 64 | 128 | 2.44 | 3.91 | 1.60 | 2.70 | 0.36 | 0.008 | 0.03 | 0.77 | 403 | 225 | 0 | — | — | 96/32 | 0 |
| w-none-c1 | PASS | 32 | 0 | 1.49 | 2.03 | 1.36 | 1.59 | 0.21 | 0.052 | 0.01 | 0.43 | 67 | 97 | 0 | 1.000 | — | 64/32 | 0 |
| w-none-c2 | PASS | 64 | 0 | 2.63 | 3.98 | 1.52 | 3.01 | 0.40 | 0.096 | 0.01 | 0.71 | 113 | 161 | 0 | 1.000 | — | 96/32 | 0 |
| w-rtmp-a | PASS | 1 | 4 | 0.11 | 0.09 | 0.82 | 0.05 | 0.03 | 0.000 | 0.04 | 0.02 | 25 | 39 | 0 | 1.000 | rtmp:ok decode-rtmp:ok | 33/32 | 0 |
| w-rtmp-b1 | PASS | 4 | 500 | 0.98 | 1.00 | 1.02 | 1.17 | 0.17 | 0.204 | 0.05 | 0.76 | 142 | 537 | 0 | 0.999 | — | 36/32 | 0 |
| w-rtmp-b2 | PASS | 4 | 1000 | 1.70 | 1.75 | 1.03 | 1.99 | 0.28 | 0.395 | 0.06 | 1.57 | 289 | 1036 | 0 | 1.000 | — | 36/32 | 0 |
| w-rtmp-c1 | PASS | 32 | 64 | 1.51 | 2.06 | 1.36 | 1.70 | 0.24 | 0.041 | 0.01 | 0.45 | 75 | 129 | 0 | 1.000 | — | 64/32 | 0 |
| w-rtmp-c2 | PASS | 64 | 128 | 2.76 | 4.06 | 1.47 | 3.19 | 0.41 | 0.115 | 0.01 | 0.77 | 129 | 225 | 0 | 1.000 | — | 96/32 | 0 |
| w-rtsp-a | PASS | 1 | 4 | 0.10 | 0.09 | 0.90 | 0.06 | 0.04 | 0.000 | 0.04 | 0.02 | 25 | 39 | 0 | 1.000 | rtsp:ok decode-rtsp:ok | 33/32 | 0 |
| w-rtsp-b1 | PASS | 4 | 500 | 1.08 | 1.19 | 1.11 | 1.25 | 0.18 | 0.208 | 0.05 | 0.76 | 155 | 536 | 0 | 1.000 | — | 36/32 | 0 |
| w-rtsp-b2 | PASS | 4 | 1000 | 1.85 | 2.14 | 1.15 | 2.22 | 0.29 | 0.405 | 0.06 | 1.58 | 309 | 1037 | 0 | 1.000 | — | 36/32 | 0 |
| w-rtsp-c1 | PASS | 32 | 64 | 1.55 | 2.09 | 1.35 | 1.66 | 0.24 | 0.067 | 0.02 | 0.46 | 72 | 128 | 0 | 1.000 | — | 64/32 | 0 |
| w-rtsp-c2 | PASS | 64 | 128 | 2.80 | 4.10 | 1.46 | 3.09 | 0.39 | 0.128 | 0.01 | 0.79 | 123 | 225 | 0 | 1.000 | — | 96/32 | 0 |
| w-whep-a2 | PASS | 1 | 4 | 0.11 | 0.10 | 0.97 | 0.11 | 0.07 | 0.006 | 0.04 | 0.03 | 22 | 37 | 0 | 1.000 | whep:ok | 33/32 | 0 |
| w-whep-b1 | PASS | 4 | 200 | 1.20 | 1.70 | 1.41 | 1.45 | 0.20 | 0.241 | 0.05 | 0.68 | 71 | 236 | 0 | 1.000 | — | 36/32 | 0 |
| w-whep-b2 | PASS | 4 | 400 | 2.23 | 3.08 | 1.38 | 2.44 | 0.34 | 0.402 | 0.05 | 1.28 | 112 | 436 | 0 | 1.000 | — | 36/32 | 0 |
| w-whep-c1 | PASS | 32 | 64 | 1.64 | 2.37 | 1.45 | 1.77 | 0.24 | 0.065 | 0.01 | 0.62 | 82 | 129 | 0 | 1.000 | — | 64/32 | 0 |
| w-whep-c2 | PASS | 64 | 128 | 3.02 | 4.69 | 1.55 | 3.60 | 0.47 | 0.175 | 0.01 | 1.06 | 136 | 224 | 0 | 1.000 | — | 96/32 | 0 |
| w-whepchrome2-b1 | PASS | 4 | 200 | 1.31 | 1.84 | 1.41 | 1.50 | 0.19 | 0.231 | 0.02 | 0.70 | 75 | 237 | 0 | 1.000 | — | 36/32 | 0 |
| w-whepchrome2-b2 | PASS | 4 | 400 | 2.32 | 3.23 | 1.39 | 2.64 | 0.33 | 0.404 | 0.01 | 1.24 | 117 | 437 | 0 | 1.000 | — | 36/32 | 0 |
| w-whepchrome2-c1 | PASS | 32 | 64 | 2.31 | 3.59 | 1.55 | 2.65 | 0.35 | 0.086 | 0.01 | 0.62 | 111 | 129 | 0 | 1.000 | — | 64/32 | 0 |
| w-whepchrome2-c2 | PASS | 64 | 128 | 4.34 | 7.10 | 1.63 | 5.32 | 0.68 | 0.178 | 0.02 | 1.07 | 199 | 225 | 0 | 1.000 | — | 96/32 | 0 |

| run | output Mbit/s (steady media received by clients) | lag span max ms | control p50/p95/p99 ms | control errors | TCP retrans | RSS MiB | threads |
|---|---|---|---|---|---|---|---|
| w-all-a | 52 | 28 | 1.3/1.6/2.8 | 0 | 0 | 39 | 16 |
| w-all-b | 1451 | 43 | 1.6/5.6/18.4 | 0 | 0 | 156 | 16 |
| w-all-c | 723 | 30 | 1.9/3.4/4.1 | 0 | 0 | 228 | 16 |
| w-allchrome2-c | 721 | 31 | 3.2/5.7/8.6 | 0 | 0 | 284 | 16 |
| w-flv-a | 9 | 2 | 1.3/1.5/1.8 | 0 | 0 | 27 | 16 |
| w-flv-b1 | 1066 | 20 | 1.6/3.1/4.9 | 0 | 21 | 105 | 16 |
| w-flv-b2 | 2131 | 41 | 1.7/6.0/7.9 | 0 | 1070 | 237 | 16 |
| w-flv-c1 | 136 | 3 | 1.6/2.5/2.8 | 0 | 0 | 71 | 16 |
| w-flv-c2 | 273 | 4 | 1.8/3.0/3.6 | 0 | 0 | 120 | 16 |
| w-gb_tcp_active-a | 9 | 2 | 1.2/1.3/1.8 | 0 | 0 | 26 | 10 |
| w-gb_tcp_active-b1 | 215 | 4 | 1.4/1.9/2.1 | 0 | 0 | 62 | 12 |
| w-gb_tcp_active-b2 | 431 | 6 | 1.5/2.2/2.8 | 0 | 0 | 89 | 12 |
| w-gb_tcp_active-c1 | 138 | 3 | 1.5/2.4/2.9 | 0 | 0 | 76 | 16 |
| w-gb_tcp_active-c2 | 276 | 4 | 1.8/3.0/3.4 | 0 | 0 | 127 | 16 |
| w-gb_tcp_passive-a | 9 | 2 | 1.2/1.4/2.2 | 0 | 0 | 26 | 10 |
| w-gb_tcp_passive-b1 | 215 | 4 | 1.3/1.8/2.2 | 0 | 0 | 60 | 12 |
| w-gb_tcp_passive-b2 | 431 | 6 | 1.5/2.1/2.6 | 0 | 0 | 90 | 12 |
| w-gb_tcp_passive-c1 | 138 | 2 | 1.5/2.3/2.7 | 0 | 0 | 76 | 16 |
| w-gb_tcp_passive-c2 | 276 | 4 | 1.8/3.0/3.8 | 0 | 0 | 128 | 16 |
| w-gb_udp-a | 9 | 2 | 1.2/1.4/1.9 | 0 | 0 | 27 | 10 |
| w-gb_udp-b1 | 431 | 19 | 1.3/2.4/3.3 | 0 | 0 | 37 | 12 |
| w-gb_udp-b2 | 862 | 50 | 1.3/5.0/26.6 | 0 | 0 | 38 | 12 |
| w-gb_udp-c1 | 138 | 3 | 1.5/2.6/3.2 | 0 | 0 | 68 | 16 |
| w-gb_udp-c2 | 276 | 4 | 1.8/2.9/3.8 | 0 | 0 | 113 | 16 |
| w-hls-a | 9 | — | 1.2/1.5/2.0 | 0 | 0 | 27 | 14 |
| w-hls-b1 | 222 | — | 1.3/1.7/2.1 | 0 | 0 | 47 | 15 |
| w-hls-b2 | 445 | — | 1.3/1.9/2.3 | 0 | 0 | 50 | 15 |
| w-hls-c1 | 140 | — | 1.5/2.4/2.9 | 0 | 0 | 219 | 16 |
| w-hls-c2 | 285 | — | 1.8/3.2/3.9 | 0 | 0 | 403 | 16 |
| w-none-c1 | 68 | 3 | 1.5/2.4/3.0 | 0 | 0 | 68 | 16 |
| w-none-c2 | 136 | 5 | 1.8/4.1/4.6 | 0 | 0 | 113 | 16 |
| w-rtmp-a | 9 | 28 | 1.3/1.5/1.9 | 0 | 0 | 27 | 14 |
| w-rtmp-b1 | 1067 | 35 | 1.6/3.1/4.7 | 0 | 53 | 144 | 16 |
| w-rtmp-b2 | 2133 | 35 | 1.6/5.8/19.1 | 0 | 96 | 291 | 16 |
| w-rtmp-c1 | 137 | 29 | 1.5/2.5/3.3 | 0 | 0 | 77 | 16 |
| w-rtmp-c2 | 273 | 30 | 1.8/3.4/3.9 | 0 | 0 | 130 | 16 |
| w-rtsp-a | 9 | 2 | 1.3/1.5/1.8 | 0 | 0 | 27 | 14 |
| w-rtsp-b1 | 1083 | 16 | 1.6/3.7/7.4 | 0 | 0 | 156 | 16 |
| w-rtsp-b2 | 2166 | 39 | 1.7/6.3/8.8 | 0 | 131 | 311 | 16 |
| w-rtsp-c1 | 139 | 3 | 1.5/2.6/3.1 | 0 | 0 | 74 | 16 |
| w-rtsp-c2 | 277 | 3 | 1.9/3.2/3.5 | 0 | 0 | 124 | 16 |
| w-whep-a2 | 9 | 2 | 1.2/1.3/1.8 | 0 | 0 | 24 | 14 |
| w-whep-b1 | 433 | 53 | 1.4/7.4/27.8 | 0 | 0 | 73 | 16 |
| w-whep-b2 | 866 | 77 | 1.6/10.0/47.7 | 0 | 0 | 115 | 16 |
| w-whep-c1 | 139 | 4 | 1.5/2.6/4.3 | 0 | 0 | 83 | 16 |
| w-whep-c2 | 277 | 6 | 2.1/3.7/5.1 | 0 | 0 | 138 | 16 |
| w-whepchrome2-b1 | 426 | 60 | 1.4/5.2/7.5 | 0 | 0 | 76 | 16 |
| w-whepchrome2-b2 | 851 | 94 | 1.5/18.5/48.0 | 0 | 0 | 118 | 16 |
| w-whepchrome2-c1 | 136 | 6 | 1.7/4.1/5.5 | 0 | 0 | 112 | 16 |
| w-whepchrome2-c2 | 272 | 8 | 3.1/5.6/7.3 | 0 | 0 | 200 | 16 |

| path | Mcycles/s per viewer (b2-b1) | Minstr/s per viewer | Mcycles/s per source incl. its viewers (c2-c1) | Mcycles/s per source excl. viewers |
|---|---|---|---|---|
| w-flv | 1.5 | 1.7 | 38.3 | 35.3 |
| w-gb_tcp_active | 1.4 | 1.6 | 39.2 | 36.4 |
| w-gb_tcp_passive | 1.3 | 1.5 | 36.3 | 33.6 |
| w-gb_udp | 2.4 | 2.8 | 39.0 | 34.2 |
| w-hls | 0.4 | 0.2 | 35.2 | 34.3 |
| w-none | — | — | 35.6 | — |
| w-rtmp | 1.4 | 1.5 | 38.9 | 36.0 |
| w-rtsp | 1.5 | 1.9 | 39.2 | 36.1 |
| w-whep | 5.2 | 6.9 | 43.3 | 33.0 |
| w-whepchrome2 | 5.1 | 6.9 | 63.5 | 53.4 |

## 附录 C：输入组合摸底

`i-<输入>-none` 只有探测观众；`i-i-<输入>-none-all` 为同一输入带全部受支持输出（重复的前缀来自作业脚本的变量覆盖，只影响命名）。`i2-*` 为复跑。

| run | status | src | viewers | Gcycles/s | Ginstr/s | IPC | busy cores | busiest core | softirq | worker spread | client cores | PSS MiB | FD | UDP drops | realtime min | probes | FD base/after | left |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| i-gbtcpa-aac-none | PASS | 16 | 0 | 0.23 | 0.17 | 0.74 | 0.33 | 0.07 | 0.047 | 0.00 | 0.42 | 34 | 64 | 0 | 0.999 | — | 48/32 | 0 |
| i-gbtcpp-aac-none | PASS | 16 | 0 | 0.22 | 0.17 | 0.77 | 0.16 | 0.06 | 0.019 | 0.01 | 0.41 | 34 | 64 | 0 | 0.999 | — | 48/32 | 0 |
| i-gbudp-aac-none | PASS | 16 | 0 | 0.24 | 0.18 | 0.77 | 0.14 | 0.04 | 0.010 | 0.00 | 0.44 | 35 | 80 | 0 | 1.000 | — | 64/32 | 0 |
| i-i-gbtcpa-aac-none-all | PASS | 16 | 168 | 1.41 | 1.73 | 1.22 | 1.61 | 0.21 | 0.149 | 0.03 | 0.94 | 115 | 232 | 0 | 0.999 | — | 48/32 | 0 |
| i-i-gbtcpp-aac-none-all | PASS | 16 | 168 | 1.36 | 1.72 | 1.26 | 1.55 | 0.20 | 0.142 | 0.01 | 0.92 | 118 | 232 | 0 | 0.999 | — | 48/32 | 0 |
| i-i-gbudp-aac-none-all | PASS | 16 | 168 | 1.39 | 1.75 | 1.25 | 1.60 | 0.21 | 0.146 | 0.03 | 0.95 | 115 | 249 | 0 | 0.999 | — | 64/32 | 0 |
| i-i-rtmp-aac-none-all | PASS | 16 | 168 | 1.23 | 1.20 | 0.97 | 1.36 | 0.18 | 0.158 | 0.03 | 0.77 | 96 | 232 | 0 | 1.000 | — | 48/32 | 0 |
| i-i-rtmp-h265-none-all | PASS | 16 | 136 | 0.49 | 0.37 | 0.75 | 0.33 | 0.06 | 0.056 | 0.02 | 0.62 | 72 | 200 | 0 | 0.999 | — | 48/32 | 0 |
| i-i-rtmp-opus-none-all | PASS | 16 | 136 | 0.64 | 0.56 | 0.87 | 0.42 | 0.07 | 0.090 | 0.02 | 0.63 | 78 | 184 | 0 | 1.000 | — | 48/32 | 0 |
| i-i-rtmp-pcma-none-all | PASS | 16 | 168 | 0.76 | 0.65 | 0.85 | 0.77 | 0.12 | 0.141 | 0.02 | 0.75 | 81 | 232 | 0 | 0.999 | — | 48/32 | 0 |
| i-i-rtmppull-aac-none-all | PASS | 16 | 168 | 1.36 | 1.68 | 1.23 | 1.45 | 0.20 | 0.134 | 0.03 | 0.95 | 109 | 232 | 0 | 0.999 | — | 48/32 | 0 |
| i-i-rtsppull-aac-none-all | PASS | 16 | 168 | 1.38 | 1.72 | 1.24 | 1.55 | 0.21 | 0.143 | 0.03 | 0.96 | 113 | 233 | 0 | 0.999 | — | 48/32 | 0 |
| i-i-rtsptcp-aac-none-all | PASS | 16 | 168 | 1.05 | 1.15 | 1.09 | 1.10 | 0.16 | 0.107 | 0.03 | 0.62 | 99 | 232 | 0 | 0.999 | — | 48/32 | 0 |
| i-i-rtsptcp-opus-none-all | PASS | 16 | 136 | 0.73 | 0.61 | 0.84 | 0.46 | 0.07 | 0.107 | 0.01 | 0.69 | 80 | 184 | 0 | 1.000 | — | 48/32 | 0 |
| i-i-rtsptcp-pcma-none-all | PASS | 16 | 168 | 0.81 | 0.68 | 0.85 | 0.62 | 0.09 | 0.114 | 0.03 | 0.78 | 81 | 232 | 0 | 0.999 | — | 48/32 | 0 |
| i-i-rtspudp-aac-none-all | PASS | 16 | 168 | 1.01 | 1.14 | 1.13 | 0.93 | 0.13 | 0.058 | 0.02 | 0.58 | 103 | 296 | 0 | 0.999 | — | 112/32 | 0 |
| i-i-wheppull-aac-none-all | PASS | 16 | 168 | 1.35 | 1.54 | 1.14 | 1.40 | 0.19 | 0.132 | 0.02 | 1.88 | 108 | 233 | 0 | 0.999 | — | 48/32 | 0 |
| i-i-whip-opus-none-all | PASS | 16 | 168 | 1.37 | 1.56 | 1.14 | 1.57 | 0.21 | 0.179 | 0.03 | 0.84 | 112 | 233 | 0 | 1.000 | — | 48/32 | 0 |
| i-rtmp-aac-none | PASS | 16 | 0 | 0.42 | 0.65 | 1.55 | 0.27 | 0.09 | 0.054 | 0.01 | 0.24 | 20 | 64 | 0 | 0.999 | — | 48/32 | 0 |
| i-rtmp-h265-none | PASS | 16 | 0 | 0.24 | 0.13 | 0.54 | 0.15 | 0.03 | 0.044 | 0.01 | 0.29 | 22 | 64 | 0 | 0.999 | — | 48/32 | 0 |
| i-rtmp-opus-none | PASS | 16 | 0 | 0.24 | 0.12 | 0.52 | 0.10 | 0.03 | 0.018 | 0.03 | 0.24 | 20 | 64 | 0 | 1.000 | — | 48/32 | 0 |
| i-rtmp-pcma-none | PASS | 16 | 0 | 0.24 | 0.13 | 0.54 | 0.16 | 0.04 | 0.040 | 0.01 | 0.24 | 20 | 64 | 0 | 0.999 | — | 48/32 | 0 |
| i-rtmppull-aac-none | PASS | 16 | 0 | 0.20 | 0.11 | 0.58 | 0.17 | 0.04 | 0.034 | 0.00 | 0.42 | 27 | 64 | 0 | 1.000 | — | 48/32 | 0 |
| i-rtsppull-aac-none | PASS | 16 | 0 | 0.21 | 0.15 | 0.69 | 0.29 | 0.06 | 0.046 | 0.00 | 0.41 | 34 | 64 | 0 | 0.999 | — | 48/32 | 0 |
| i-rtsptcp-aac-none | PASS | 16 | 0 | 0.20 | 0.13 | 0.63 | 0.17 | 0.05 | 0.022 | 0.01 | 0.21 | 22 | 64 | 0 | 0.999 | — | 48/32 | 0 |
| i-rtsptcp-opus-none | PASS | 16 | 0 | 0.30 | 0.17 | 0.56 | 0.30 | 0.06 | 0.047 | 0.03 | 0.29 | 22 | 64 | 0 | 1.000 | — | 48/32 | 0 |
| i-rtsptcp-pcma-none | PASS | 16 | 0 | 0.25 | 0.15 | 0.59 | 0.21 | 0.07 | 0.015 | 0.02 | 0.26 | 22 | 64 | 0 | 0.999 | — | 48/32 | 0 |
| i-rtspudp-aac-none | PASS | 16 | 0 | 0.19 | 0.13 | 0.69 | 0.12 | 0.03 | 0.015 | 0.01 | 0.21 | 27 | 128 | 0 | 0.999 | — | 112/32 | 0 |
| i-wheppull-aac-none | PASS | 16 | 0 | 0.81 | 1.03 | 1.27 | 0.89 | 0.12 | 0.026 | 0.00 | 1.30 | 47 | 64 | 0 | 0.999 | — | 48/32 | 0 |
| i-whip-opus-none | PASS | 16 | 0 | 0.78 | 1.03 | 1.31 | 0.84 | 0.12 | 0.017 | 0.01 | 0.26 | 47 | 65 | 0 | 1.000 | — | 48/32 | 0 |
| i2-rtmp-aac-none | PASS | 16 | 0 | 0.24 | 0.14 | 0.57 | 0.17 | 0.04 | 0.046 | 0.02 | 0.24 | 20 | 64 | 0 | 0.999 | — | 48/32 | 0 |
| i2-rtsptcp-aac-none | PASS | 16 | 0 | 0.19 | 0.13 | 0.65 | 0.07 | 0.02 | 0.023 | 0.01 | 0.21 | 21 | 64 | 0 | 0.999 | — | 48/32 | 0 |

| run | output Mbit/s (steady media received by clients) | lag span max ms | control p50/p95/p99 ms | control errors | TCP retrans | RSS MiB | threads |
|---|---|---|---|---|---|---|---|
| i-gbtcpa-aac-none | 34 | 51 | 1.4/1.7/1.9 | 0 | 0 | 35 | 16 |
| i-gbtcpp-aac-none | 34 | 52 | 1.4/1.7/1.9 | 0 | 0 | 35 | 16 |
| i-gbudp-aac-none | 34 | 63 | 1.4/1.7/1.9 | 0 | 0 | 36 | 16 |
| i-i-gbtcpa-aac-none-all | 362 | 70 | 1.6/3.3/4.8 | 0 | 0 | 116 | 16 |
| i-i-gbtcpp-aac-none-all | 362 | 64 | 1.6/3.4/4.1 | 0 | 0 | 120 | 16 |
| i-i-gbudp-aac-none-all | 362 | 64 | 1.6/3.0/3.7 | 0 | 0 | 116 | 16 |
| i-i-rtmp-aac-none-all | 344 | 60 | 1.5/2.8/4.3 | 0 | 0 | 97 | 16 |
| i-i-rtmp-h265-none-all | 278 | 193 | 1.5/1.9/2.1 | 0 | 0 | 73 | 16 |
| i-i-rtmp-opus-none-all | 285 | 42 | 1.4/1.9/2.3 | 0 | 0 | 79 | 16 |
| i-i-rtmp-pcma-none-all | 352 | 62 | 1.5/1.8/2.2 | 0 | 0 | 83 | 16 |
| i-i-rtmppull-aac-none-all | 362 | 65 | 1.6/3.3/5.0 | 0 | 0 | 110 | 16 |
| i-i-rtsppull-aac-none-all | 362 | 75 | 1.6/3.4/5.0 | 0 | 0 | 114 | 16 |
| i-i-rtsptcp-aac-none-all | 344 | 98 | 1.5/23.5/47.6 | 0 | 0 | 100 | 16 |
| i-i-rtsptcp-opus-none-all | 285 | 37 | 1.5/1.8/2.8 | 0 | 0 | 81 | 16 |
| i-i-rtsptcp-pcma-none-all | 352 | 42 | 1.5/1.9/2.2 | 0 | 0 | 82 | 16 |
| i-i-rtspudp-aac-none-all | 344 | 103 | 1.5/12.8/58.5 | 0 | 0 | 104 | 16 |
| i-i-wheppull-aac-none-all | 362 | 75 | 1.6/2.4/2.7 | 0 | 0 | 109 | 16 |
| i-i-whip-opus-none-all | 361 | 29 | 1.5/2.2/2.7 | 0 | 0 | 113 | 16 |
| i-rtmp-aac-none | 32 | 60 | 1.3/1.6/1.9 | 0 | 0 | 21 | 16 |
| i-rtmp-h265-none | 32 | 63 | 1.3/1.6/1.8 | 0 | 0 | 23 | 16 |
| i-rtmp-opus-none | 33 | 42 | 1.3/1.6/1.8 | 0 | 0 | 21 | 16 |
| i-rtmp-pcma-none | 33 | 61 | 1.3/1.6/1.8 | 0 | 0 | 21 | 16 |
| i-rtmppull-aac-none | 34 | 75 | 1.4/1.6/2.0 | 0 | 0 | 28 | 16 |
| i-rtsppull-aac-none | 34 | 67 | 1.4/1.7/2.0 | 0 | 0 | 35 | 16 |
| i-rtsptcp-aac-none | 32 | 58 | 1.3/1.5/1.7 | 0 | 0 | 23 | 16 |
| i-rtsptcp-opus-none | 33 | 37 | 1.3/1.6/1.9 | 0 | 0 | 23 | 16 |
| i-rtsptcp-pcma-none | 33 | 42 | 1.3/1.6/1.9 | 0 | 0 | 23 | 16 |
| i-rtspudp-aac-none | 32 | 24 | 1.3/1.6/2.2 | 0 | 0 | 28 | 16 |
| i-wheppull-aac-none | 34 | 74 | 1.5/2.1/2.4 | 0 | 0 | 49 | 16 |
| i-whip-opus-none | 34 | 2 | 1.3/2.1/2.7 | 0 | 0 | 48 | 16 |
| i2-rtmp-aac-none | 32 | 61 | 1.3/1.6/1.8 | 0 | 0 | 21 | 16 |
| i2-rtsptcp-aac-none | 32 | 58 | 1.3/1.6/1.8 | 0 | 0 | 24 | 16 |

| path | Mcycles/s per viewer (b2-b1) | Minstr/s per viewer | Mcycles/s per source incl. its viewers (c2-c1) | Mcycles/s per source excl. viewers |
|---|---|---|---|---|

## 附录 D：容量搜索全部运行

| run | status | src | viewers | Gcycles/s | Ginstr/s | IPC | busy cores | busiest core | softirq | worker spread | client cores | PSS MiB | FD | UDP drops | realtime min | probes | FD base/after | left |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| cap-all-288-r1 | PASS | 288 | 1512 | 15.37 | 22.00 | 1.43 | 6.51 | 0.82 | 0.428 | 0.02 | 5.63 | 956 | 1976 | 0 | 0.999 | — | 320/32 | 0 |
| cap-all-320-r1 | PASS | 320 | 1680 | 16.81 | 24.29 | 1.44 | 7.19 | 0.90 | 0.451 | 0.02 | 5.99 | 1045 | 2193 | 0 | 0.999 | — | 352/32 | 0 |
| cap-all-352-r1 | PASS | 352 | 1848 | 18.30 | 26.44 | 1.45 | 7.87 | 0.99 | 0.541 | 0.03 | 6.09 | 1142 | 2409 | 0 | 0.999 | — | 384/32 | 0 |
| cap-all-352-r2 | PASS | 352 | 1848 | 18.29 | 26.45 | 1.45 | 7.87 | 0.99 | 0.536 | 0.01 | 6.12 | 1150 | 2409 | 0 | 0.999 | — | 384/32 | 0 |
| cap-all-384-r1 | FAIL | 384 | 2016 | 18.67 | 27.94 | 1.50 | 8.00 | 1.00 | 0.491 | 0.00 | 6.01 | 1370 | 2625 | 321131 | 0.990 | — | 416/32 | 0 |
| cap-flv-320-r1 | PASS | 320 | 640 | 12.35 | 20.07 | 1.63 | 5.89 | 0.74 | 0.187 | 0.00 | 3.72 | 479 | 993 | 0 | 0.999 | — | 352/32 | 0 |
| cap-flv-384-r1 | PASS | 384 | 768 | 14.75 | 23.86 | 1.62 | 6.33 | 0.79 | 0.182 | 0.02 | 4.62 | 562 | 1185 | 0 | 0.999 | — | 416/32 | 0 |
| cap-flv-416-r1 | PASS | 416 | 832 | 15.89 | 25.78 | 1.62 | 6.79 | 0.85 | 0.203 | 0.02 | 5.20 | 602 | 1281 | 0 | 0.999 | — | 448/32 | 0 |
| cap-flv-448-r1 | PASS | 448 | 896 | 16.81 | 27.55 | 1.64 | 7.24 | 0.91 | 0.216 | 0.01 | 5.59 | 647 | 1377 | 0 | 0.999 | — | 480/32 | 0 |
| cap-flv-480-r1 | PASS | 480 | 960 | 17.85 | 29.31 | 1.64 | 7.73 | 0.97 | 0.243 | 0.01 | 6.00 | 695 | 1473 | 0 | 0.999 | — | 512/32 | 0 |
| cap-flv-480-r2 | PASS | 480 | 960 | 17.87 | 29.33 | 1.64 | 7.74 | 0.97 | 0.238 | 0.01 | 5.90 | 695 | 1473 | 0 | 1.000 | — | 512/32 | 0 |
| cap-flv-512-r1 | FAIL | 512 | 1024 | 18.42 | 30.53 | 1.66 | 8.00 | 1.00 | 0.223 | 0.00 | 5.64 | 850 | 1569 | 331369 | 0.989 | — | 544/32 | 0 |
| cap-gb_tcp_active-320-r1 | PASS | 320 | 640 | 12.56 | 19.57 | 1.56 | 5.99 | 0.75 | 0.212 | 0.01 | 3.49 | 506 | 993 | 0 | 0.999 | — | 352/32 | 0 |
| cap-gb_tcp_active-384-r1 | PASS | 384 | 768 | 14.54 | 23.33 | 1.60 | 6.18 | 0.77 | 0.177 | 0.02 | 4.18 | 593 | 1185 | 0 | 0.999 | — | 416/32 | 0 |
| cap-gb_tcp_active-416-r1 | PASS | 416 | 832 | 15.64 | 25.17 | 1.61 | 6.66 | 0.84 | 0.220 | 0.02 | 4.67 | 634 | 1281 | 0 | 1.000 | — | 448/32 | 0 |
| cap-gb_tcp_active-448-r1 | PASS | 448 | 896 | 16.57 | 26.89 | 1.62 | 7.11 | 0.89 | 0.219 | 0.01 | 4.91 | 680 | 1377 | 0 | 0.999 | — | 480/32 | 0 |
| cap-gb_tcp_active-480-r1 | PASS | 480 | 960 | 17.51 | 28.60 | 1.63 | 7.56 | 0.95 | 0.249 | 0.01 | 5.33 | 725 | 1473 | 0 | 0.999 | — | 512/32 | 0 |
| cap-gb_tcp_active-480-r2 | PASS | 480 | 960 | 17.53 | 28.62 | 1.63 | 7.57 | 0.95 | 0.257 | 0.00 | 5.45 | 725 | 1473 | 0 | 0.999 | — | 512/32 | 0 |
| cap-gb_tcp_active-512-r1 | FAIL | 512 | 1024 | 18.40 | 29.97 | 1.63 | 8.00 | 1.00 | 0.227 | 0.00 | 5.37 | 825 | 1569 | 8573 | 0.988 | — | 544/32 | 0 |
| cap-gb_tcp_passive-320-r1 | PASS | 320 | 640 | 12.55 | 19.53 | 1.56 | 5.94 | 0.75 | 0.199 | 0.00 | 3.52 | 502 | 993 | 0 | 0.999 | — | 352/32 | 0 |
| cap-gb_tcp_passive-384-r1 | PASS | 384 | 768 | 14.57 | 23.36 | 1.60 | 6.21 | 0.78 | 0.194 | 0.02 | 4.21 | 593 | 1185 | 0 | 0.999 | — | 416/32 | 0 |
| cap-gb_tcp_passive-416-r1 | PASS | 416 | 832 | 15.52 | 25.12 | 1.62 | 6.61 | 0.83 | 0.208 | 0.02 | 4.53 | 635 | 1281 | 0 | 0.999 | — | 448/32 | 0 |
| cap-gb_tcp_passive-448-r1 | PASS | 448 | 896 | 16.54 | 26.88 | 1.62 | 7.09 | 0.89 | 0.244 | 0.01 | 4.97 | 679 | 1377 | 0 | 0.999 | — | 480/32 | 0 |
| cap-gb_tcp_passive-480-r1 | PASS | 480 | 960 | 17.52 | 28.64 | 1.63 | 7.57 | 0.95 | 0.211 | 0.01 | 5.17 | 729 | 1473 | 0 | 0.999 | — | 512/32 | 0 |
| cap-gb_tcp_passive-480-r2 | PASS | 480 | 960 | 17.57 | 28.61 | 1.63 | 7.59 | 0.95 | 0.248 | 0.01 | 5.35 | 730 | 1473 | 0 | 0.999 | — | 512/32 | 0 |
| cap-gb_tcp_passive-512-r1 | FAIL | 512 | 1024 | 18.40 | 29.92 | 1.63 | 8.00 | 1.00 | 0.222 | 0.00 | 4.91 | 879 | 1569 | 48901 | 0.987 | — | 544/32 | 0 |
| cap-gb_udp-320-r1 | PASS | 320 | 640 | 12.43 | 20.35 | 1.64 | 5.96 | 0.75 | 0.206 | 0.00 | 3.48 | 443 | 1633 | 0 | 0.999 | — | 352/32 | 0 |
| cap-gb_udp-384-r1 | PASS | 384 | 768 | 14.78 | 24.26 | 1.64 | 6.30 | 0.79 | 0.239 | 0.02 | 4.32 | 519 | 1953 | 0 | 1.000 | — | 416/32 | 0 |
| cap-gb_udp-416-r1 | PASS | 416 | 832 | 15.82 | 26.15 | 1.65 | 6.78 | 0.85 | 0.240 | 0.01 | 4.74 | 555 | 2113 | 0 | 1.000 | — | 448/32 | 0 |
| cap-gb_udp-448-r1 | PASS | 448 | 896 | 16.82 | 28.01 | 1.67 | 7.24 | 0.91 | 0.256 | 0.01 | 5.14 | 596 | 2273 | 0 | 0.999 | — | 480/32 | 0 |
| cap-gb_udp-480-r1 | PASS | 480 | 960 | 17.80 | 29.79 | 1.67 | 7.70 | 0.96 | 0.289 | 0.01 | 5.77 | 639 | 2433 | 0 | 0.999 | — | 512/32 | 0 |
| cap-gb_udp-480-r2 | PASS | 480 | 960 | 17.76 | 29.80 | 1.68 | 7.68 | 0.96 | 0.290 | 0.01 | 5.48 | 644 | 2433 | 0 | 0.999 | — | 512/32 | 0 |
| cap-gb_udp-512-r1 | FAIL | 512 | 1024 | 18.47 | 31.23 | 1.69 | 8.00 | 1.00 | 0.282 | 0.00 | 5.57 | 782 | 2593 | 304537 | 0.990 | — | 544/32 | 0 |
| cap-hls-320-r1 | PASS | 320 | 640 | 11.39 | 19.34 | 1.70 | 5.79 | 0.73 | 0.021 | 0.01 | 3.54 | 1837 | 993 | 0 | — | — | 352/32 | 0 |
| cap-hls-384-r1 | PASS | 384 | 768 | 13.24 | 23.20 | 1.75 | 5.87 | 0.74 | 0.016 | 0.00 | 4.09 | 2172 | 1184 | 0 | — | — | 416/32 | 0 |
| cap-hls-448-r1 | PASS | 448 | 896 | 15.33 | 26.90 | 1.75 | 6.53 | 0.82 | 0.019 | 0.04 | 4.76 | 2536 | 1376 | 0 | — | — | 480/32 | 0 |
| cap-hls-480-r1 | PASS | 480 | 960 | 16.18 | 28.67 | 1.77 | 6.93 | 0.87 | 0.020 | 0.04 | 4.89 | 2721 | 1472 | 0 | — | — | 512/32 | 0 |
| cap-hls-512-r1 | PASS | 512 | 1024 | 17.05 | 30.40 | 1.78 | 7.35 | 0.92 | 0.021 | 0.04 | 5.20 | 2902 | 1569 | 0 | — | — | 544/32 | 0 |
| cap-hls-544-r1 | PASS | 544 | 1088 | 18.17 | 32.14 | 1.77 | 7.89 | 0.99 | 0.022 | 0.02 | 5.65 | 3106 | 1665 | 0 | — | — | 576/32 | 0 |
| cap-hls-544-r2 | PASS | 544 | 1088 | 18.17 | 32.13 | 1.77 | 7.89 | 1.00 | 0.023 | 0.03 | 5.51 | 3114 | 1665 | 0 | — | — | 576/32 | 0 |
| cap-hls-576-r1 | FAIL | 576 | 1152 | 18.42 | 33.06 | 1.79 | 8.00 | 1.00 | 0.021 | 0.00 | 5.87 | 3263 | 1761 | 832280 | — | — | 608/32 | 0 |
| cap-rtmp-320-r1 | PASS | 320 | 640 | 12.42 | 19.86 | 1.60 | 5.98 | 0.75 | 0.185 | 0.01 | 3.42 | 545 | 993 | 0 | 0.999 | — | 352/32 | 0 |
| cap-rtmp-384-r1 | PASS | 384 | 768 | 14.72 | 23.76 | 1.61 | 6.29 | 0.79 | 0.189 | 0.01 | 4.29 | 642 | 1185 | 0 | 0.999 | — | 416/32 | 0 |
| cap-rtmp-416-r1 | PASS | 416 | 832 | 15.66 | 25.56 | 1.63 | 6.70 | 0.84 | 0.211 | 0.02 | 4.61 | 690 | 1281 | 0 | 1.000 | — | 448/32 | 0 |
| cap-rtmp-448-r1 | PASS | 448 | 896 | 16.62 | 27.35 | 1.65 | 7.15 | 0.90 | 0.250 | 0.01 | 4.78 | 750 | 1377 | 0 | 1.000 | — | 480/32 | 0 |
| cap-rtmp-480-r1 | PASS | 480 | 960 | 17.67 | 29.12 | 1.65 | 7.64 | 0.96 | 0.255 | 0.01 | 5.61 | 805 | 1473 | 0 | 0.999 | — | 512/32 | 0 |
| cap-rtmp-480-r2 | PASS | 480 | 960 | 17.68 | 29.11 | 1.65 | 7.65 | 0.96 | 0.261 | 0.01 | 5.24 | 801 | 1473 | 0 | 0.999 | — | 512/32 | 0 |
| cap-rtmp-512-r1 | FAIL | 512 | 1024 | 18.42 | 30.51 | 1.66 | 8.00 | 1.00 | 0.237 | 0.00 | 5.30 | 951 | 1569 | 216134 | 0.990 | — | 544/32 | 0 |
| cap-rtsp-320-r1 | PASS | 320 | 640 | 12.39 | 20.16 | 1.63 | 5.90 | 0.74 | 0.170 | 0.00 | 3.44 | 528 | 993 | 0 | 1.000 | — | 352/32 | 0 |
| cap-rtsp-384-r1 | PASS | 384 | 768 | 14.81 | 23.96 | 1.62 | 6.34 | 0.80 | 0.190 | 0.02 | 4.21 | 621 | 1184 | 0 | 1.000 | — | 416/32 | 0 |
| cap-rtsp-416-r1 | PASS | 416 | 832 | 15.86 | 25.84 | 1.63 | 6.78 | 0.85 | 0.209 | 0.01 | 4.65 | 674 | 1281 | 0 | 0.999 | — | 448/32 | 0 |
| cap-rtsp-448-r1 | PASS | 448 | 896 | 16.85 | 27.63 | 1.64 | 7.26 | 0.91 | 0.209 | 0.01 | 4.98 | 719 | 1377 | 0 | 0.999 | — | 480/32 | 0 |
| cap-rtsp-480-r1 | PASS | 480 | 960 | 17.85 | 29.42 | 1.65 | 7.72 | 0.97 | 0.246 | 0.01 | 5.46 | 769 | 1473 | 0 | 0.999 | — | 512/32 | 0 |
| cap-rtsp-480-r2 | PASS | 480 | 960 | 17.79 | 29.39 | 1.65 | 7.71 | 0.97 | 0.252 | 0.00 | 5.13 | 769 | 1473 | 0 | 0.999 | — | 512/32 | 0 |
| cap-rtsp-512-r1 | FAIL | 512 | 1024 | 18.42 | 30.70 | 1.67 | 8.00 | 1.00 | 0.233 | 0.00 | 5.19 | 928 | 1569 | 301210 | 0.992 | — | 544/32 | 0 |
| cap-whep-320-r1 | PASS | 320 | 640 | 14.75 | 23.44 | 1.59 | 6.29 | 0.79 | 0.277 | 0.01 | 4.01 | 614 | 993 | 0 | 1.000 | — | 352/32 | 0 |
| cap-whep-352-r1 | PASS | 352 | 704 | 16.11 | 25.65 | 1.59 | 6.83 | 0.86 | 0.312 | 0.02 | 4.55 | 673 | 1089 | 0 | 1.000 | — | 384/32 | 0 |
| cap-whep-384-r1 | PASS | 384 | 768 | 17.17 | 27.73 | 1.62 | 7.36 | 0.92 | 0.335 | 0.01 | 4.53 | 738 | 1185 | 0 | 1.000 | — | 416/32 | 0 |
| cap-whep-384-r2 | PASS | 384 | 768 | 17.20 | 27.75 | 1.61 | 7.38 | 0.92 | 0.346 | 0.01 | 4.60 | 740 | 1185 | 0 | 1.000 | — | 416/32 | 0 |
| cap-whep-416-r1 | PASS | 416 | 832 | 18.47 | 29.77 | 1.61 | 8.00 | 1.00 | 0.370 | 0.00 | 5.24 | 793 | 1281 | 0 | 1.000 | — | 448/32 | 0 |
| cap-whep-416-r2 | FAIL | 416 | 832 | 18.48 | 29.72 | 1.61 | 8.00 | 1.00 | 0.361 | 0.00 | 5.28 | 820 | 1281 | 9718 | 0.998 | — | 448/32 | 0 |
| cap-whep-448-r1 | FAIL | 448 | 896 | 18.66 | 31.22 | 1.67 | 8.00 | 1.00 | 0.344 | 0.00 | 5.78 | 976 | 1377 | 556019 | 0.990 | — | 480/32 | 0 |
| cap-whepchrome-224-r1 | PASS | 224 | 448 | 15.50 | 24.84 | 1.60 | 6.67 | 0.84 | 0.182 | 0.01 | 2.77 | 640 | 705 | 0 | 1.000 | — | 256/32 | 0 |
| cap-whepchrome-256-r1 | PASS | 256 | 512 | 17.38 | 28.14 | 1.62 | 7.50 | 0.94 | 0.221 | 0.01 | 2.93 | 737 | 801 | 0 | 1.000 | — | 288/32 | 0 |
| cap-whepchrome-256-r2 | PASS | 256 | 512 | 17.39 | 28.14 | 1.62 | 7.51 | 0.94 | 0.214 | 0.01 | 2.91 | 743 | 801 | 0 | 1.000 | — | 288/32 | 0 |
| cap-whepchrome-288-r1 | FAIL | 288 | 576 | 18.48 | 31.02 | 1.68 | 8.00 | 1.00 | 0.234 | 0.00 | 3.62 | 881 | 897 | 281385 | 0.990 | — | 320/32 | 0 |
| cap-whepchrome-320-r1 | FAIL | 320 | 640 | 18.53 | 32.30 | 1.74 | 8.00 | 1.00 | 0.223 | 0.00 | 3.95 | 1013 | 993 | 933961 | 0.023 | — | 352/32 | 0 |

| run | output Mbit/s (steady media received by clients) | lag span max ms | control p50/p95/p99 ms | control errors | TCP retrans | RSS MiB | threads |
|---|---|---|---|---|---|---|---|
| cap-all-288-r1 | 3255 | 33 | 2.8/4.6/5.3 | 0 | 0 | 957 | 16 |
| cap-all-320-r1 | 3617 | 62 | 3.0/4.5/5.4 | 0 | 0 | 1046 | 16 |
| cap-all-352-r1 | 3979 | 146 | 4.2/6.4/8.1 | 0 | 0 | 1143 | 16 |
| cap-all-352-r2 | 3979 | 146 | 4.3/6.8/8.1 | 0 | 0 | 1151 | 16 |
| cap-all-384-r1 | 4205 | 904 | 10.3/17.1/22.6 | 0 | 0 | 1371 | 16 |
| cap-flv-320-r1 | 1364 | 5 | 2.8/4.2/5.1 | 0 | 0 | 481 | 16 |
| cap-flv-384-r1 | 1637 | 7 | 3.2/4.6/5.1 | 0 | 0 | 563 | 16 |
| cap-flv-416-r1 | 1774 | 9 | 3.5/5.3/6.0 | 0 | 0 | 604 | 16 |
| cap-flv-448-r1 | 1910 | 11 | 3.8/6.1/6.8 | 0 | 0 | 649 | 16 |
| cap-flv-480-r1 | 2046 | 90 | 4.4/6.2/7.1 | 0 | 0 | 697 | 16 |
| cap-flv-480-r2 | 2046 | 83 | 4.2/6.2/6.9 | 0 | 0 | 697 | 16 |
| cap-flv-512-r1 | 2131 | 876 | 10.8/17.6/21.9 | 0 | 0 | 851 | 16 |
| cap-gb_tcp_active-320-r1 | 1379 | 8 | 2.9/4.9/6.6 | 0 | 0 | 508 | 16 |
| cap-gb_tcp_active-384-r1 | 1655 | 10 | 3.0/4.4/4.8 | 0 | 2 | 595 | 16 |
| cap-gb_tcp_active-416-r1 | 1793 | 7 | 3.3/5.0/6.3 | 0 | 0 | 636 | 16 |
| cap-gb_tcp_active-448-r1 | 1930 | 9 | 3.6/5.2/6.0 | 0 | 0 | 682 | 16 |
| cap-gb_tcp_active-480-r1 | 2068 | 95 | 3.8/5.5/6.4 | 0 | 0 | 727 | 16 |
| cap-gb_tcp_active-480-r2 | 2068 | 157 | 3.9/5.8/6.8 | 0 | 0 | 727 | 16 |
| cap-gb_tcp_active-512-r1 | 2200 | 858 | 10.8/13.7/14.8 | 0 | 0 | 827 | 16 |
| cap-gb_tcp_passive-320-r1 | 1379 | 27 | 3.0/4.4/5.3 | 0 | 0 | 504 | 16 |
| cap-gb_tcp_passive-384-r1 | 1655 | 6 | 3.0/4.3/6.2 | 0 | 0 | 595 | 16 |
| cap-gb_tcp_passive-416-r1 | 1793 | 11 | 3.1/4.9/5.7 | 0 | 0 | 637 | 16 |
| cap-gb_tcp_passive-448-r1 | 1930 | 89 | 3.5/4.6/5.3 | 0 | 0 | 681 | 16 |
| cap-gb_tcp_passive-480-r1 | 2068 | 44 | 3.8/5.5/6.6 | 0 | 0 | 730 | 16 |
| cap-gb_tcp_passive-480-r2 | 2068 | 25 | 3.9/5.6/6.4 | 0 | 1 | 732 | 16 |
| cap-gb_tcp_passive-512-r1 | 2194 | 909 | 10.3/15.6/19.1 | 0 | 0 | 881 | 16 |
| cap-gb_udp-320-r1 | 1379 | 5 | 2.9/4.5/5.4 | 0 | 0 | 445 | 16 |
| cap-gb_udp-384-r1 | 1655 | 10 | 3.1/4.5/5.1 | 0 | 0 | 521 | 16 |
| cap-gb_udp-416-r1 | 1793 | 9 | 3.4/5.2/6.0 | 0 | 0 | 557 | 16 |
| cap-gb_udp-448-r1 | 1930 | 8 | 3.6/5.1/6.1 | 0 | 0 | 598 | 16 |
| cap-gb_udp-480-r1 | 2068 | 97 | 4.4/6.4/7.3 | 0 | 0 | 641 | 16 |
| cap-gb_udp-480-r2 | 2068 | 154 | 4.8/7.2/7.9 | 0 | 0 | 646 | 16 |
| cap-gb_udp-512-r1 | 2158 | 845 | 11.0/18.3/20.6 | 0 | 0 | 783 | 16 |
| cap-hls-320-r1 | 1423 | — | 3.1/5.6/6.5 | 0 | 0 | 1838 | 16 |
| cap-hls-384-r1 | 1709 | — | 2.9/4.4/5.0 | 0 | 0 | 2172 | 16 |
| cap-hls-448-r1 | 1988 | — | 3.3/5.0/5.9 | 0 | 0 | 2536 | 16 |
| cap-hls-480-r1 | 2135 | — | 3.6/5.5/6.7 | 0 | 0 | 2721 | 16 |
| cap-hls-512-r1 | 2276 | — | 4.1/5.8/7.7 | 0 | 0 | 2900 | 16 |
| cap-hls-544-r1 | 2422 | — | 5.6/9.7/11.3 | 0 | 0 | 3109 | 16 |
| cap-hls-544-r2 | 2420 | — | 6.3/10.4/12.5 | 0 | 1 | 3116 | 16 |
| cap-hls-576-r1 | 2441 | — | 11.2/21.9/24.5 | 0 | 0 | 3264 | 16 |
| cap-rtmp-320-r1 | 1365 | 35 | 2.9/4.5/5.2 | 0 | 0 | 547 | 16 |
| cap-rtmp-384-r1 | 1639 | 35 | 3.2/4.6/5.7 | 0 | 0 | 644 | 16 |
| cap-rtmp-416-r1 | 1775 | 35 | 3.3/5.3/6.1 | 0 | 0 | 692 | 16 |
| cap-rtmp-448-r1 | 1912 | 34 | 3.7/5.7/6.6 | 0 | 0 | 752 | 16 |
| cap-rtmp-480-r1 | 2048 | 46 | 4.3/6.1/7.1 | 0 | 0 | 806 | 16 |
| cap-rtmp-480-r2 | 2048 | 51 | 4.0/6.0/6.8 | 0 | 0 | 803 | 16 |
| cap-rtmp-512-r1 | 2152 | 821 | 10.8/17.1/19.6 | 0 | 0 | 954 | 16 |
| cap-rtsp-320-r1 | 1386 | 6 | 2.8/4.0/5.2 | 0 | 0 | 529 | 16 |
| cap-rtsp-384-r1 | 1664 | 5 | 3.1/4.4/4.7 | 0 | 0 | 622 | 16 |
| cap-rtsp-416-r1 | 1802 | 18 | 3.3/4.7/5.5 | 0 | 0 | 675 | 16 |
| cap-rtsp-448-r1 | 1941 | 114 | 3.8/5.7/6.8 | 0 | 0 | 721 | 16 |
| cap-rtsp-480-r1 | 2079 | 84 | 4.4/6.5/7.6 | 0 | 0 | 771 | 16 |
| cap-rtsp-480-r2 | 2079 | 72 | 4.2/6.0/7.0 | 0 | 0 | 770 | 16 |
| cap-rtsp-512-r1 | 2171 | 858 | 10.6/16.3/18.4 | 0 | 0 | 930 | 16 |
| cap-whep-320-r1 | 1386 | 5 | 2.8/4.1/5.2 | 0 | 0 | 615 | 16 |
| cap-whep-352-r1 | 1525 | 8 | 3.2/4.9/5.8 | 0 | 0 | 675 | 16 |
| cap-whep-384-r1 | 1663 | 39 | 3.5/5.3/6.7 | 0 | 0 | 739 | 16 |
| cap-whep-384-r2 | 1663 | 97 | 3.3/5.1/5.9 | 0 | 0 | 741 | 16 |
| cap-whep-416-r1 | 1803 | 344 | 9.8/12.9/15.5 | 0 | 0 | 795 | 16 |
| cap-whep-416-r2 | 1800 | 834 | 10.3/14.1/16.8 | 0 | 0 | 822 | 16 |
| cap-whep-448-r1 | 1840 | 955 | 11.3/20.4/28.1 | 0 | 0 | 977 | 16 |
| cap-whepchrome-224-r1 | 953 | 8 | 3.5/5.9/7.3 | 0 | 0 | 641 | 16 |
| cap-whepchrome-256-r1 | 1090 | 10 | 4.3/6.9/7.8 | 0 | 0 | 739 | 16 |
| cap-whepchrome-256-r2 | 1090 | 20 | 4.2/7.2/8.3 | 0 | 0 | 744 | 16 |
| cap-whepchrome-288-r1 | 1180 | 934 | 11.7/30.2/37.6 | 0 | 0 | 882 | 16 |
| cap-whepchrome-320-r1 | 1170 | 1219 | 16.2/35.2/54.4 | 0 | 0 | 1015 | 16 |

| path | Mcycles/s per viewer (b2-b1) | Minstr/s per viewer | Mcycles/s per source incl. its viewers (c2-c1) | Mcycles/s per source excl. viewers |
|---|---|---|---|---|
