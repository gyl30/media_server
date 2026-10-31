# 协议回归补强与按需 Opus→AAC（2026-10-31）

## 结论

| 阶段 | 内容 | 结论 |
| --- | --- | --- |
| A | H.265→WHEP、GB 三种传输端到端解码、`transcoders.bt` 放行 | 完成，已提交 `4b09c66`。完整 Release 矩阵 31/31（基线 PCMU 项为工具限制，修正后 PASS） |
| B | WHIP / WHEP Pull 的 Opus→AAC 只在 AAC 流有存活订阅者时执行 | **保留**。5 个文件 +49/−17 行，只新增一个只读查询，无跨 worker 同步；纯直通 WHEP 最大持续容量 384→1056 源（2.75×），无观看者 CPU −79%；需要 AAC 的负载不变 |
| C | Chrome 默认 WHEP offer 的直通 | 可行，停在评审：最小方案是删除直通条件中的 `stereo=1` 要求（[`c-passthrough-experiment.diff`](c-passthrough-experiment.diff)）。实验未改生产代码 |

最终回答：

1. **矩阵剩余缺口**：GB 输出的 G.711μ 音频无法用 FFmpeg 验证（只验证视频）；H.265→WHEP 只有原生客户端证据，真实浏览器 H.265 解码未验证；RTSP 输入的 AAC 在 RTMP/RTSP/FLV/HLS/GB 输出上有既有的约 10 ms 音频时间戳跳变（28–29 处，GB 计 53 处，基线同样存在，见 [已有问题](#已有问题)）；客户端工具 `gb_client`/`gb_fanout` 的改动未在 sanitizer 下运行。
2. **按需 Opus→AAC 能否简单实现**：能。接收器在源所属 worker 上逐帧查询分发表是否有未过期的订阅者，无订阅者时跳过转码，恢复时用原配置重启转码器。没有新增注册表、计数或唤醒。
3. **真实容量收益**：只对“全部观众都直通”的负载成立。64 源×2 直通 WHEP 每秒 3.04→1.42 Gcycles；纯直通 WHEP 最大持续 384→1056 源，留余量（最忙核 ≤0.87）352→960 源。Chrome 默认 offer、FLV、全输出混合不变。
4. **首个 AAC 观众是否受影响**：无可测影响。每次加入转码器重启 1 次，首个音频帧仍早于或等于首个视频帧（0–9 ms，常开版本 0–17 ms），A/V 偏差 p50 与常开版本相差 ≤2 ms，无音频缺口，已有直通 WHEP 观众的音频连续。
5. **Chrome 直通能否安全实现**：能，且比“单声道直通”更简单：不需要按实际声道登记轨道，也不需要动态切换。RFC 7587 规定接收端无论 `stereo` 取值都必须能解码立体声；真实 Chrome 在单声道、立体声源下直通均无隐藏，切换源的 2 次隐藏经对照确认来自素材拼接而非声道切换。基线对这些观众本来就经 egress 输出单声道，直通不会减少声道，还少一代有损编码。实施前还需容量实测和真实 Chrome 推流的切换验证。
6. **是否需要继续研究热点**：直通 WHEP 去掉转码后，每源成本只剩 SRTP、逐包 `sendto` 与 RTP 解析；1088 源的失败点是 8 核饱和后 UDP 丢包，没有单核瓶颈或 worker 不均。现阶段不需要新的热点专项；若要再提升，方向是 Chrome 默认 offer 的 egress（阶段 C）而不是转发路径。

## 1. 版本、构建与环境

- 基线 `a`：`8ce9f5c` 生产源码（二进制 sha256 前缀 `b95752ba`）。
- 阶段 B `b`：`8ce9f5c` + 本报告的 `media/` 改动（`11c1fe9b`），同一依赖、同一 RelWithDebInfo 配置。
- 阶段 C `c`：`8ce9f5c` + [`c-passthrough-experiment.diff`](c-passthrough-experiment.diff)（`d5406924`），只用于实验，不进入生产。
- Sanitizer：`b` 的 ASan、TSan Debug 构建。
- 测试机 `<TEST_HOST>`，netns 内 DUT 198.19.0.10、客户端 198.19.0.11；netns `ip_local_port_range` 32768–49151，媒体端口池 49152–65535 不变，未改宿主机 sysctl。
- 性能点 DUT 固定 CPU 0–7（SMT 兄弟核空闲），8 worker；客户端 8–15、24–31。功能矩阵与 sanitizer 为 4 worker、CPU 0–3。每次运行前 `env_check.sh` 确认无残留进程。
- 实验二进制放在独立目录并保持名称 `media_server`（`pgrep -x` 与残留检查依赖进程名）。

## 2. 阶段 A：协议回归补强

### 改动（`4b09c66`）

- `regression/protocol_matrix.py`：H.265 两个场景（RTMP、RTSP TCP 的 H.265+AAC）改用全部输出，包含 WHEP；`fault-source-stop` 增加 GB TCP 主动、被动输出；摘要增加 `video_frames`、`audio_frames`、`audio_unverified`。
- `bench/gb_fanout.cpp`、`bench/clients/gb_client.*`：`--dump-file` 记录第一个收到媒体的观众的 PS 负载。`gb_client` 原本已逐包校验 RTP 序号连续与 PS PTS 单调。
- `bench/perf_run.py`：`--av-probe` 时为每种 GB 传输的第一组记录 PS，窗口后交给 `av_probe.py` 解复用、解码，结果写入 `av_probe.gb_*`；源音频为 G.711μ 时只解码视频并标注 `audio_unverified`。
- `bench/av_probe.py`：增加 `first_audio_s`、`audio_start_after_video_ms`，以及 `--video-only`。
- `.gitignore`：只放行 `20261030-protocol-matrix-performance/transcoders.bt`。

### 结果

完整 Release 矩阵（`mx-a`、`mx-b`，各 31 项）：`b` 31/31 PASS；`a` 30/31，`rtmp-h264-pcmu` FAIL。原因是 FFmpeg 的 MPEG-PS 解复用器只在 IMKH 文件中把流类型 0x91 映射为 G.711μ，GB 输出的音频帧为 0、视频正常。工具改为只解码视频后重跑（r09）PASS，音频标注为未验证。

GB 输出（三种传输结果一致）：

| 场景 | 音频帧 | 同步标记 | A/V p50 ms | 音频 PTS 跳变 |
| --- | --- | --- | --- | --- |
| rtmp-h264-aac | 1787 | 19 | 3.0 | 0 |
| rtmp-h264-bframes-aac | 1783 | 19 | 0.3 | 0 |
| rtmp-h265-aac | 1783 | 19 | 12.3 | 0 |
| rtmp-h264-pcma | 1783 | 19 | 11.0 | 0 |
| rtmp-h264-pcmu | 未验证 | — | — | — |
| rtsp-tcp/udp-h264-aac | 1782 | 19 | 7.3 | 53（既有，见下） |
| rtsp-tcp-h265-aac | 1782 | 19 | 19.3 | 53（既有） |
| whip-h264-opus | 1783–1787 | 19 | −6.0（a）/ −6.3（b） | 0 |
| gb-udp / tcp 输入 | 1783 | 19 | 0.7 | 0 |
| fault-source-stop | 989 | 11 | 3.0 | 0 |

- RTP 序号与 PS 解复用：`gb_client` 逐包校验，任何不连续都会使组失败；全部 PASS。
- H.265→WHEP：RTMP 3.7 ms、RTSP 9.7 ms（A/V p50），SDP 协商 H.265 profile/tier/level、视频 RTP、AAC→Opus egress、回放解码与清理均在矩阵内通过。只有原生客户端证据，浏览器端 H.265 未验证。
- 源停止：GB TCP 主动、被动组均以 `SOURCE_ENDED` 结束，FD 回落，无残留接收器。
- `transcoders.bt`：在 r04 中对 `a`、`b` 两个二进制实际运行（`bpftrace transcoders.bt <binary>`），探针全部挂上并给出启动、销毁和 egress 计数。

## 3. 阶段 B：按需 Opus→AAC

### 设计

WHIP 与 WHEP Pull 共用 `whip_media_receiver`。主流（AAC）的订阅者都需要 AAC：FLV tag 输出（RTMP/FLV）、RTSP、HLS 切片器、PS 输出（GB）、WHEP 的 AAC 回放与 `whep_audio_egress`；直通 WHEP 只订阅 `opus_stream()`。因此“主流是否有存活订阅者”就是“是否需要 AAC”，并不是“有任意 sink”。唯一的例外是不带音频的 WHEP offer 仍订阅主流，结果是多做一次转码，不影响正确性。

改动（`media/` 5 个文件）：

- `worker_sink_dispatcher::has_live_sinks()`：任一组内有未过期的弱引用即为真。
- `media_stream::has_sinks()` 转发上述查询。
- `whip_media_receiver` 保存转码配置；每个音频帧先查询 `has_sinks()`，为假时跳过并标记未激活；恢复后的第一帧用原配置重新 `startup()`。

线程：接收器、源 `media_stream` 及其分发表都在源所属 worker 上，订阅也在该 worker 上登记（`add_sink` 投递到所属 worker），因此查询无竞争，不存在丢失唤醒。重启而不是暂停是必须的：转码器输出 PTS 由累计样本数决定，暂停后继续会让时间线落后。重启后时间线与 `initial_padding` 补偿和源起始时一样。

实验中发现并修复了一个问题：最初用分发表是否为空来判断，但 `flv_tag_output` 销毁时不会主动退订，残留的过期弱引用让转码一直开着（RTSP 加入时转码器重启 0 次）。改为检查未过期的订阅者后正确。

### 正确性

| 用例 | 证据 | 结果 |
| --- | --- | --- |
| 无观看者 | r10：64 源，基线每源 1 个转码器，`b` 不转码 | PASS |
| 直通 WHEP | r04、r05：转码器只在就绪探测期间启动 | PASS |
| Chrome 默认 offer WHEP | egress 订阅主流，`b` 正常转码；A/V 与 `a` 一致 | PASS |
| RTMP / FLV / RTSP / HLS / GB UDP / GB TCP | r05 单源单输出 `a`/`b` 对照，解码、A/V、跳变计数一致 | PASS |
| 全部输出 | r05 `av-all`、`av-pull-all` | PASS |
| 先直通后 AAC 加入、AAC 全部离开再加入 | `aac_join`：一个直通观众，FLV、RTSP、RTMP 轮流加入，间隔 6 s 无 AAC 消费者 | PASS |
| 源停止、以新 stream_id 重建 | 矩阵 `fault-source-stop`（Release、ASan、TSan） | PASS |
| WHIP SIGTERM / SIGKILL | `lc-asan`、`lc-tsan` 发布轮换 8 轮，含 SIGTERM（3 ms 内源消失）与 SIGKILL（20 s 超时） | PASS，无残留观众 |
| WHEP Pull 同上 | `aac_join --input whep_pull`、矩阵 `whep-pull`（Release、ASan、TSan） | PASS |

AAC 观众加入（`aac_join`，Release）：

| 运行 | 转码器启动 | 每次加入 | 首音频相对首视频 ms（FLV/RTSP/RTMP） | A/V p50 ms | 直通观众音频跳变 |
| --- | --- | --- | --- | --- | --- |
| whip-a | 1 | 0 | −11 / 0 / −11 | −5.5 / −4.0 / −5.5 | 0 |
| whip-b | 4 | 1 | −5 / 0 / −9 | −5.0 / −4.7 / −4.3 | 0 |
| pull-a | 1 | 0 | −17 / 0 / −17 | 3.5 / 1.0 / 3.5 | 0 |
| pull-b | 4 | 1 | −6 / 0 / 0 | 2.5 / 3.0 / 3.5 | 0 |

负数表示音频先于视频到达。按需版本首音频不晚于视频，没有额外等待；A/V 偏差与常开版本相差在 2 ms 内。RTSP 输出在客户端起始处的 1 个音频跳变在所有版本都有。fdk 在重启时打印 “2 frames left in the queue on closing”，这是丢弃旧编码器内部缓存的预期行为。

Sanitizer（`b`）：矩阵子集 9 项、生命周期与 `aac_join` 两种输入在 ASan、TSan 下全部 PASS；ASan 日志只有 swapcontext 的固定警告，TSan 无报告。

### 性能 A/B（8 核，60 s 窗口，各 2 轮）

| 负载 | a Gcycles/s | b Gcycles/s | a Ginstr/s | b Ginstr/s | 控制 p99 ms a / b | RSS MiB a / b |
| --- | --- | --- | --- | --- | --- | --- |
| 64 WHIP，无观看者（r10） | 2.09 | 0.44 | 3.65 | 0.43 | — | PSS 92 / 82 |
| 64 WHIP × 2 直通 WHEP | 3.04 | 1.42 | 4.69 | 1.49 | 5.7 / 3.7 | 138 / 141 |
| 32 WHEP Pull × 2 直通 WHEP | 1.55 | 0.78 | 2.35 | 0.79 | 4.3 / 3.2 | 83 / 85 |
| 64 WHIP × 2 Chrome offer WHEP | 4.32 | 4.31 | 7.10 | 7.09 | 7.5 / 7.6 | 199 / 202 |
| 64 WHIP × 2 FLV | 2.77 | 2.79 | 4.09 | 4.09 | 3.9 / 4.1 | 120 / 125 |
| 64 WHIP 全输出（336 观看者） | 3.61 | 3.61 | 5.07 | 5.05 | 4.8 / 4.6 | 227 / 231 |

全部运行 PASS，UDP 丢包、TCP 重传、控制错误均为 0，输出码率 a、b 相同。

`perf_run.py --outputs none` 会为每源保留一个持续的 HTTP-FLV 观看者确认推进，这本身是 AAC 消费者，因此“无观看者”改用 [`idle_sources.py`](idle_sources.py) 只按 `/receivers` 判定就绪（r10，两轮）。r04 的 `ab-none-*`（2.69 / 2.66 Gcycles/s）实际是“每源 1 个 FLV”，不作为无观看者结论。

bpftrace 显示 `b` 的转码器启动只发生在就绪探测阶段（探测客户端反复加入、离开），测量窗口内为 0，没有抖动式重启。

### 容量（纯直通 WHEP，每源 2 观看者）

| 版本 | 最大持续 | 最忙核 | 下一档 | 留余量（最忙核 ≤0.87） |
| --- | --- | --- | --- | --- |
| a | 384 源 / 768（本轮复测 PASS） | 0.925 | 上一轮 416 1/2、448 FAIL | 352（上一轮） |
| b | 1056 源 / 2112（2/2 PASS） | 0.919–0.926 | 1088 FAIL：8 核饱和，UDP 丢包 298843 | 960（0.846） |

`b` 在 1056 源时 PSS 约 2.0 GiB、FD 3201。576–832 源之间服务端 CPU 时间稳定在约 5.8 核，这是 schedutil 低负载时降频造成的计量偏差；cycles 与最忙核随负载单调上升。

回归点：Chrome 默认 offer WHEP 256 源、FLV 480 源在 `b` 下 PASS（最忙核 0.948、0.977），与上一轮容量一致。

### 决定

保留。收益在同一环境下可重复，正确性用例与 sanitizer 全部通过，改动只在接收器内部，没有改动订阅接口或协议职责。代价是 AAC 消费者在空闲后加入时重启一次转码器（fdk 与 libopus 初始化），没有测到时延或音画影响。

## 阶段 C：Chrome 默认 offer 直通可行性

### 现状与依据

- 直通条件（`opus_passthrough_compatible`）：源轨道为 2 声道时要求 offer 带 `stereo=1`，`maxplaybackrate` 缺省或 48000、`maxaveragebitrate` 缺省或 510000、`maxptime` 缺省或 120。WHIP 与 WHEP Pull 源的 Opus 轨道一律登记为 2 声道。
- Chrome 默认 offer 为 `minptime=10;useinbandfec=1`，不带 `stereo=1`，因此走 AAC→Opus egress。egress 的输出声道由 offer 决定，不带 `stereo=1` 时为 1 声道：**基线已经把立体声源以单声道发给 Chrome**，并且经过 Opus→AAC→Opus 两代有损编码。
- RFC 7587：`stereo` 只表示接收端偏好，缺省为 0；“无论 stereo 参数如何，每个接收端都必须能接收并解码立体声信号”。`sprop-stereo` 只是提示，声道由每个包的 TOC 字节在带内表示，rtpmap 固定为 `opus/48000/2`。
- WHIP 应答对 Opus 不带 fmtp（即不表示接收端偏好立体声），因此 Chrome 推流时发送单声道；FFmpeg 等推流端照样发送立体声。

### 实验

[`c_channels.py`](c_channels.py) 同时发布 3 路 WHIP 源：单声道 64 kbps、立体声 128 kbps、单声道→立体声→单声道（40 s 切换一次，分段重新编码后拼接）。每路源挂 3 个 `whep_fanout` 观众（`stereo=0`、Chrome 默认 fmtp、Firefox 默认 fmtp）、1 个真实 Chrome（每 2 s 采样 inbound-rtp 统计）和 1 个 FLV 解码。对比基线 `a` 与删除 `stereo=1` 要求的实验构建 `c`（作业 r12；r08、r11 因素材拼接和 playwright 路径失败，已修正重跑）。

| 源 | 观众 offer | a：路径 / 收到的 TOC | c：路径 / 收到的 TOC | 解码（回放） |
| --- | --- | --- | --- | --- |
| 单声道 | stereo=0、Chrome | egress / 单声道 | 直通 / 单声道 | a、c 均无缺口、无警告 |
| 立体声 | stereo=0、Chrome | egress / **单声道** | 直通 / 立体声 | 同上 |
| 立体声 | Firefox | 直通 / 立体声 | 直通 / 立体声 | 同上 |
| 切换 | stereo=0、Chrome | egress / 单声道 | 直通 / 按源切换 | 2 处重叠、4 个警告，与 a 的 Firefox 直通相同 |

egress 启动次数：a 为 3（每源 1 个，输出 1 声道），c 为 0。

真实 Chrome（WHEP，默认 offer）：

| 源 | a 隐藏样本 / 事件 | c 隐藏样本 / 事件 | 丢包 |
| --- | --- | --- | --- |
| 单声道 | 0 / 0 | 0 / 0 | 0 |
| 立体声 | 0 / 0 | 0 / 0 | 0 |
| 切换 | 0 / 0 | 1050 / 2（Chrome 时间 36.4 s、74.9 s，即源的两个切换点） | 0 |
| 对照：同样三段拼接、全程立体声（r13，仅 c） | — | 930 / 2（36.5 s、74.9 s） | 0 |

对照组不切换声道，也在相同两个拼接点出现同样的隐藏，说明隐藏来自分段编码拼接处的时间戳重叠（离线素材的问题），而不是直通时的声道切换。基线没有出现，是因为 egress 重新编码抹掉了重叠。切换源在所有输出上的 A/V 偏差约 −28 ms，FLV 也一样，同为素材造成。

### 结论与最小方案

可行，停在评审，未进入生产代码。

- 方案：在 `opus_passthrough_compatible` 中删除 2 声道源对 `stereo=1` 的要求，其余条件不变（[`c-passthrough-experiment.diff`](c-passthrough-experiment.diff)）。不需要按实际声道登记轨道，不需要检测声道变化，不需要在直通与 egress 之间动态切换，因为接收端必须处理带内声道切换。
- 音质与声道：对 Chrome 默认 offer，基线输出单声道 64 kbps 的二次转码结果，直通发送原始 Opus，声道不减少，少一代有损编码。Chrome 在 offer 不带 `stereo=1` 时通常以单声道解码；本次 inbound 统计只给出 rtpmap 的 2 声道，未测到实际播放声道，因此这一点未验证。
- 收益：Chrome 默认 offer 的观众不再需要每源 egress（上一份报告测得每源约 20 Mcycles/s），叠加阶段 B 后，Chrome 观众的负载应接近纯直通。本轮未做容量实测，不作数字结论。
- 实施前还需：容量 A/B；由真实 Chrome 推流并在推流中切换声道的验证（本次切换源是离线素材）；应答中的 `sprop-stereo` 目前按输出声道给出（Chrome 直通时为 0），应改为按源登记的声道给出，或说明它只是提示；同步更新 `docs/protocols.md` 中关于 Chrome 走 egress 的描述。

## 已有问题

- RTSP 输入的 AAC 源在 RTMP/RTSP/FLV/HLS 输出上有 28–29 处约 10 ms 的音频 PTS 跳变（GB 解码帧更多，计 53 处）；WHEP 经重新编码，为 0。与视频编码、本轮改动无关，上一轮 `mx-rel` 中已经存在，上一份报告漏记。
- RTSP 输出在客户端起始处有 1 个音频 PTS 跳变，所有版本都有。

## 复现

作业脚本在 [`jobs/`](jobs/)，按名称顺序执行；包装脚本 [`run_perf.sh`](run_perf.sh)、[`lib.sh`](lib.sh) 指定运行目录与 `SERVER_BIN`。

| 作业 | 内容 |
| --- | --- |
| r01、r02 | 阶段 A smoke；`aac_join` 的 whip/whep_pull × a/b |
| r03 | 完整 Release 矩阵 a/b |
| r04 | A/B 性能（带 bpftrace） |
| r05 | 单源各输出 A/V a/b |
| r06 | 容量搜索 |
| r07 | ASan、TSan |
| r08 | 阶段 C 声道实验（含素材生成；r11、r12 为修正后的重跑，以 r12 为准） |
| r09 | PCMU 矩阵项重跑 |
| r10 | 无观看者 A/B |
| r13 | 阶段 C 对照：同样分段拼接、全程立体声 |

成本表沿用上一份报告的 [`costs.py`](../20261030-protocol-matrix-performance/costs.py)。原始结果、bpftrace 输出与浏览器数据只在测试机。
