# GB28181 解复用验证、RTSP AAC 时间线调查与 Chrome Opus 直通

日期 2026-10-31。被测生产代码为 `b72e703`（工作区只改测试工具，生产源码与 HEAD 一致）。测试机 `<TEST_HOST>`，8 核 DUT（CPU 0–7，SMT 兄弟核空闲），客户端绑定其他核；netns 内 `ip_local_port_range` 为 32768–49151，媒体端口池 49152–65535，每次运行前由环境检查确认，未改宿主机全局 sysctl。原始日志在测试机 `runs4/` 下，路径见各节。

## 结论

| 阶段 | 内容 | 结论 |
| --- | --- | --- |
| A | GB PS 解复用与完整音画验证 | 新增 `gb_ps_demux`（复用 libmpeg）与 `gb_av_probe.py`；AAC、G.711A、G.711μ 均独立解复用、按 PSM 识别编码、解码并与素材逐帧/逐字节一致；三种传输 A/V 同步通过；负向变异全部被拒绝。删除 `audio_unverified` |
| B | GB 测试客户端 ASan/TSan | 106 次 harness 运行、矩阵 GB 场景、多观看者、dump 写入/关闭/失败、`gb_ps_demux` 负向输入均无 sanitizer 报告；日志通道经正向对照确认有效 |
| C | RTSP AAC 约 10 ms PTS 跳变 | 第一次出现在服务端 RTSP 接收解包：同一 RTP 包内的后续 AU 按整数毫秒 21 ms 递增，下一包按 RTP 时间戳重新对齐，形成每 33 帧一次 +10.67 ms 的跳变。无 PCM 缺口、无丢帧、无长期漂移。建议暂不修复 |
| D | Chrome 默认 WHEP Opus 直通 | 属于 RFC 7587 多目的地网关的 SHOULD NOT；Chrome 推流源为单声道不偏离，立体声源每观众多约 38 kbps 且 Chrome 下混播放。64 源×2 Chrome WHEP cycles −67%，容量 256→1056 源。实验未合入，建议单独评审 |

## 阶段 A：GB28181 PS 解复用与音画验证

### 工具

- [`bench/gb_ps_demux.cpp`](../../../bench/gb_ps_demux.cpp)：读取 `gb_fanout --dump-file` 记录的完整 RTP 包（2 字节大端长度 + RTP，RFC 4571 分帧），逐包校验 RTP 版本、PT、SSRC 和序号连续，把负载交给服务端同一个 libmpeg `ps_demuxer`。编码只按 PSM 的 stream_type 识别（0x1b/0x24/0x0f/0x90/0x91），没有 PSM 映射或映射变化即报错，不按 PES stream_id 猜 G.711（libmpeg 无 PSM 时会把 0xC0 当 AAC）。检查视频为 Annex-B、AAC 为整帧 ADTS、PTS 存在、33 位 DTS 回绕展开后严格递增、PTS ≥ DTS；输出各路 ES、每帧 `pts/dts/key/offset/bytes/units` 的 CSV 和 JSON 摘要，任一错误退出码非 0。
- [`bench/gb_av_probe.py`](../../../bench/gb_av_probe.py)：ffmpeg 只按已知编码解码 ES（`-f h264|hevc`、`-f aac`、`-f alaw|mulaw -ar 8000 -ac 1`）；视频帧亮度、音频 10 ms RMS 都放在 PS PTS 时间线上找闪白和蜂鸣，不用 ffmpeg 为裸 ES 生成的时间戳。检查解码帧数等于 PS 帧数、解码样本数等于帧数×每帧样本（AAC 1024、G.711 每字节 1 样本）、音频 PTS 相邻步长与样本时长的偏差不超过 5 ms。`--reference` 把输出音频与素材逐帧（AAC，ADTS 负载）或逐字节（G.711）比对：用连续 100 个单元定位，候选位置不唯一时加倍窗口直到唯一，之后顺序比对，统计不匹配与跳变。
- [`regression/gb_ps_negative.py`](../../../regression/gb_ps_negative.py)：对正常 dump 做 6 种变异，要求原样 dump 通过、所有变异被拒绝。
- `gb_client` 的 dump 改为整包记录，打不开时 `configure` 返回失败（`gb_fanout` 退出码 2），结束时显式关闭并检查写入状态（`phase=dump closed=`，失败计入退出码）。`perf_run.py` 改调 `gb_av_probe.py`；`av_probe.py` 去掉了只为 GB 加的 `--video-only`；矩阵摘要用 `audio_codec/audio_samples/checks/reference` 取代 `audio_unverified`。

### 结果

4 种编码 × 3 种传输（作业 `s01`，探测用最终工具重跑，`runs4/mx-gb-codecs`）：

| 源 | 视频帧（解码/PS） | 音频帧 | 解码样本 = 期望 | 闪白/蜂鸣配对 | A/V p50 | 与素材比对 |
| --- | --- | --- | --- | --- | --- | --- |
| H.264 + PCMU | 1141/1141 | 1784 | 304469 = 304469 | 19 | 0.6 ms | 1784 单元逐字节一致，0 跳变 |
| H.264 + PCMA | 1140/1140 | 1783 | 一致 | 19 | 0.6 ms | 一致，0 跳变 |
| H.264 + AAC | 1144/1144 | 1787 | 1829888 = 1829888 | 19 | 9.3 ms | 一致，0 跳变 |
| H.265 + AAC | 1141/1141 | 1783 | 一致 | 19 | 15.3 ms | 一致，0 跳变 |

UDP、TCP active、TCP passive 三种传输的结果相同；音频 RMS 约 −25 dB，无静音段；PTS 步长偏差 −0.375～0.75 ms，无缺口、无重叠。PCMU 先单独跑并通过，再跑全部 GB 场景。

负向（4 份 dump：PCMU/UDP、AAC/TCP passive、H.265/TCP active、PCMA/TCP active；ASan/TSan 构建另跑 2 份）：原样 dump 全部通过，以下变异全部被拒绝：

| 变异 | 失败的检查 |
| --- | --- |
| 删除全部音频包（序号重排） | audio_present |
| PSM 音频 stream_type 改为 0 | demux（不支持的 stream_type） |
| 3 个音频 PES 末尾截掉 8 字节 | AAC：demux（ADTS 不完整）；G.711：reference |
| RTP 版本非法 | demux |
| 中间缺 1 个 RTP 包 | demux（序号不连续） |
| 25 个音频包负载按字节异或 | reference（AAC 另有 audio_decoded） |

限制：G.711 PES 被截断时 libmpeg 把后续字节并入，解复用器本身不报错，只能靠逐字节比对发现。

完整 Release 矩阵（`s04c`，`runs4/mx-full`）：28/31 PASS。GB 探测在 AAC、G.711A、G.711μ、H.265、B 帧、GB 输入、RTMP/RTSP/WHEP 拉流、WHIP、多源跨 worker、源中断场景全部通过逐项检查；其中素材为 AAC/G.711 且非 WebRTC 输入的场景同时通过素材比对（WHIP、WHEP 拉流源的音频经 Opus→AAC 转码，不做比对）。3 个 FAIL 是 RTSP 输入的 AAC 源（`rtsp-tcp-h264-aac`、`rtsp-udp-h264-aac`、`rtsp-tcp-h265-aac`），失败项只有 GB 的 `audio_timeline`：53 处、最大 10.667 ms 的 PTS 跳变，样本数与素材比对都通过，即阶段 C 的已知现象。同一场景的 RTMP/RTSP/FLV/HLS 输出也测到 28–29 处，但 av_probe 只记录不判定；本轮不为掩盖它放宽阈值，是否把它列为已知例外取决于 C 的处理决定。

矩阵过程中修正了两处工具误判：读取 dump 时误用 `eof()` 导致全部失败（`s01` 第一次运行，结果已作废）；周期性素材在起始窗口不唯一时锚定到更早的周期，在 GB 输入场景报 1 次“跳变”（期望位置 137、实际 512，相差 375 帧 = 8 s）——改为窗口不唯一时加长。

## 阶段 B：GB 测试客户端 ASan/TSan

构建：Debug、`MEDIA_SERVER_SANITIZER=address` 与 `thread`，目标 `gb_fanout`、`gb_ps_demux`、`media_server`，编译无警告。ASan 使用 `detect_leaks=1`，两者都用 `log_path` 输出到各运行目录（`runs4/san-gb-asan|tsan/`）。

| 覆盖 | 方式 | ASan | TSan |
| --- | --- | --- | --- |
| 三种传输的正常接收、8 观看者、dump 写入与关闭 | [`gb_client_harness.py`](gb_client_harness.py) 假控制面 + 回放真实 RTP/PS | 退出 0，dump 可被 `gb_ps_demux`（sanitizer 构建）完整解析 | 同左 |
| 15 种非法/截断包 × 3 传输：1 字节、11 字节、版本 1、CSRC 越界、扩展越界、非法 padding、错误 PT、错误 SSRC、序号跳跃、无 pack 的续包、pack 头截断、PES PTS 截断、空负载、60 KB 超大包、PES 长度越界 | 同上，400 个正常包后注入 | 45/45 退出 1，客户端判为测量前失败，无信号退出 | 45/45 同左 |
| 源异常结束（UDP 停发；TCP 发 RST） | 同上 | UDP 判为媒体不推进，TCP 判为测量前失败 | 同左 |
| dump 写入失败 | `--dump-file /dev/full` | 退出 1，`phase=dump closed=0` | 同左 |
| dump 无法打开 | 不存在的目录 | 退出 2，配置失败 | 同左 |
| 真实服务端：PCMU/H.265 矩阵项、GB UDP/TCP active/TCP passive 输入、源中断 | `protocol_matrix.py --client-dir san-*` | 通过（GB 输入 3 项曾因上述锚点误判失败，修正后用新工具复核 9 份 dump 全部通过） | 全部通过 |
| 真实服务端多观看者 | `perf_run.py` RTMP 2 源 × 三种 GB 输出各 4 观看者，30 s | PASS，PCMU 探测 ok | PASS |
| `gb_ps_demux` 非法输入 | 三种传输 dump + 2 组负向变异 | 全部按预期，0 报告 | 同左 |

所有运行的 sanitizer 报告文件数为 0。为排除“日志没写出来”，作业 `s02c` 用相同的调用方式加 `verbosity=1`，ASan/TSan 的 `gb_fanout`（经 harness 环境传递）和 `gb_ps_demux` 都在 `log_path` 写出了运行时日志，二进制也确认链接了对应 sanitizer 运行时。客户端退出码和失败阶段只用于判断行为是否符合预期，sanitizer 结论以日志为准。

## 阶段 C：RTSP AAC 约 10 ms PTS 跳变

[`rtsp_aac_layers.py`](rtsp_aac_layers.py) 对同一源逐层取时间戳（作业 `s04b`，`runs4/layers-*`，每种输入 20 s）。素材 `avsync_720p30.flv`（AAC 48 kHz，每帧 21.333 ms）。

| 层 | RTSP UDP 推流 | RTSP TCP 推流 | RTMP 推流（对照） |
| --- | --- | --- | --- |
| 1 素材（ffprobe，FLV 毫秒） | 21/22 ms 交替，无 >26 ms 步长 | 同左 | 同左 |
| 2 FFmpeg 推流 RTP（UDP 抓包） | 47 包，每包 33 个 AU；时间戳步长 33792 = 33×1024（1 包少 8 个样本） | 未抓包（TCP 交错） | — |
| 3/4 服务端解包后的内部帧（GB PES，90 kHz） | 1184 个 21 ms（1890 tick）+ 36 个 32 ms（2880），第一处在第 33 帧 | 同左 | 812 个 21 ms + 406 个 22 ms，无跳变 |
| 5 RTMP/FLV/RTSP 输出（ffprobe） | 909 个 21 ms + 28 个 32 ms（RTSP 29 个，另有起始处 1 个） | 同左 | 21/22 ms，无跳变（RTSP 起始 1 个） |
| 6 GB PES 时间线偏差 | −0.333～+10.667 ms，36 处 >5 ms | 同左 | −0.333～+0.667 ms |
| 7 解码样本 | 1250304 = 期望；与素材逐帧比对 1221/1221 匹配，0 跳变 | 同左 | 同左 |
| A/V（闪白−蜂鸣，PS PTS） | 4.0～14.3 ms | 同左 | 4.3～5.0 ms |

根因：FFmpeg 的 RTSP 推流按约 0.7 s 的最大打包延迟把 33 个 AAC AU 放进一个 RTP 包（RFC 3640 AU-headers），RTP 时间戳正确。服务端用 ireader librtp 解包，[`rtp-mpeg4-generic-unpack.c`](../../../third/ireader/librtp/source/payload/rtp-mpeg4-generic-unpack.c) 对包内每个 AU 用同一个 RTP 时间戳回调；[`avpbs-aac.c`](../../../third/ireader/avbsf/src/avpbs-aac.c) 遇到不大于上一帧的 PTS 时按 `1000*1024/sample_rate` 递增，整数除法得到 21 ms 而不是 21.333 ms。一个包内 33 帧累计少 10.67 ms，下一个包的首帧按真实 RTP 时间戳重新对齐，于是每 33 帧出现一次 +10.67 ms 的步长。

- 第一处出现的层：服务端 RTSP 接收解包（第 3 层）。素材与推流 RTP 都正确，排除源缺口；之后各层原样继承，输出侧没有引入新的跳变。
- 幅度与频率：每次 +10.67 ms，每 33 帧（704 ms）一次，约 1.42 次/秒；20 s 窗口 28–30 次，GB 探测的约 38 s 窗口 53 次。
- PCM：没有真实缺口。帧数、解码样本数、与素材逐帧比对都完整，跳变只在时间戳上。
- 长期影响：每包重新对齐，误差不累积，没有漂移；音频时间戳相对真实位置在 0～−10.67 ms 间锯齿变化，A/V 偏差比 RTMP 输入多出同样幅度。WHEP 为 0 是因为 AAC→Opus egress 按样本数重建时间线。
- 排除项：不是素材缺口（第 1、2 层正确）、不是重采样（无转码路径）、不是工具误判（ffprobe 对三种输出和 PS PTS 两条独立路径测到同一周期，RTMP 对照为 0）。毫秒量化本身只会造成 21/22 交替（RTMP 对照），不会造成 32 ms 步长。
- 是否修复：误差上限 10.67 ms，低于常用的唇音同步可察觉范围（ITU-R BT.1359 约 −125～+45 ms），播放器按 PTS 播放时最多表现为一次 10 ms 量级的插静音或丢样，对单帧以上的连续性无影响。建议暂不修复。如要修复，应在接收侧按 RTP 时间戳 + 包内序号 × 1024 计算每个 AU 的时间戳，而不是在 `third/` 中改动 avpbs；这是生产时间戳代码改动，需要单独评审，本轮未改。

## 阶段 D：Chrome 默认 WHEP Opus 直通

### RFC 7587 的约束

- §3.4、§6.1：`stereo` 表示接收端偏好，缺省 0（偏好单声道）；“无论 stereo 参数如何，每个接收端都必须能接收并解码立体声信号”；向偏好单声道的接收端发送立体声“可能导致不必要的”网络与处理开销。`sprop-stereo` 是发送端提示，1 表示可能发送立体声，0 表示“很可能只发单声道”，接收端据此可以安全下混、不必以立体声运行处理链。
- §7.1：`stereo` 是单向的接收参数。“发送给单一目的地时，`stereo` 为 0 的情况下发送端 MUST NOT 使用立体声。”“向多个目的地发送同一编码音频的网关或发送端，在 `stereo` 为 0 时 SHOULD NOT 使用立体声，因为这会导致网络资源的低效使用。”`stereo` 不影响互通；`sprop-stereo` 同样不影响互通。
- 声道在每个包的 TOC 字节中带内表示，rtpmap 固定为 `opus/48000/2`。

### media_server 伴随流直通的归类

WHIP 源的原始 Opus 进入伴随流，所有直通的 WHEP 会话发送的是同一份编码负载（不重新编码，只各自 SRTP 加密），媒体服务器不针对某个接收端编码。这是 §7.1 所说的“向多个目的地发送同一编码音频的网关”，适用 SHOULD NOT，而不是单一目的地的 MUST NOT。

是否偏离 SHOULD NOT 取决于源的实际声道：

- Chrome 等遵守 §7.1 的 WHIP 推流端：服务端 WHIP 应答对 Opus 不带 `stereo=1`（缺省 0），推流端作为单一目的地的发送端 MUST NOT 发送立体声，源本身就是单声道。直通给 `stereo=0` 的观众时没有发送立体声，不构成偏离。
- FFmpeg、本项目 `whip_publisher` 等仍发送立体声的推流端，以及 WHEP 拉流源（服务端向上游 offer `stereo=1`）：直通会向 `stereo=0` 的观众发送立体声，偏离 SHOULD NOT。RFC 给出的理由只有网络效率，并明确不影响互通。可选的合规做法是保留每源一个 AAC→Opus egress（基线行为），代价是每源一次解码 + 编码的 CPU、多一代有损编码和编码延迟。

本实验接受这一偏离，依据为：（1）接收端按 MUST 必须能解码立体声，互通不受影响，下文用真实 Chrome 验证；（2）偏离只发生在源为立体声时，额外开销是源码率与 egress 单声道 64 kbps 之差，与视频码率相比占比小，下文按实测给出；（3）CPU 收益按源计，带宽代价按观众计，二者的权衡由下文的 A/B 和容量数据决定。如果不接受这一偏离，应保留旧行为。

### 实验改动

[`d-passthrough-experiment.diff`](d-passthrough-experiment.diff)，只在实验构建中应用，未进入工作区生产代码：

- `opus_passthrough_compatible` 删除“2 声道源要求 offer 带 `stereo=1`”，去掉随之不再使用的参数；`maxplaybackrate`（缺省或 48000）、`maxaveragebitrate`（缺省或 510000）、`a=maxptime`（缺省或 120）三项限制保持不变。
- 应答的 `sprop-stereo`：直通时取源轨道声道数（WHIP/WHEP 拉流源登记为 2，即提示“可能发送立体声”），egress 时仍取 egress 输出声道数。原实现在直通给 Chrome 时会填 0，与可能收到的立体声不符。

### 性能 A/B（8 核，64 WHIP 源，60 s 窗口，各 2 轮）

`a` 为当前生产构建（rt-s `build/media_server`，含阶段 A/B 的工具），`d` 为 `a` 加上述补丁，构建类型、编译器、依赖相同。WHEP 观众全部使用 Chrome 默认 fmtp `minptime=10;useinbandfec=1`；WHIP 源为 `perf_fixture_whip.mkv`（Opus 2 声道）。表中为两轮均值，[`ab_table.py`](ab_table.py) 从结果和 bpftrace 提取（作业 `s05`；`ab-chrome-a-1` 在 `s05` 开头被环境检查拦下，由 `s09` 补跑）。

| 负载 | Gcycles/s a → d | Ginstr/s a → d | 服务端核 a → d | 最忙核 a → d | 控制 p99 ms a → d | PSS MiB a → d |
| --- | --- | --- | --- | --- | --- | --- |
| ×2 Chrome WHEP | 4.32 → 1.44 | 7.10 → 1.49 | 5.21 → 1.53 | 0.667 → 0.214 | 7.0 → 3.5 | 201 → 141 |
| ×2 Chrome WHEP + ×2 FLV | 4.76 → 3.51 | 7.44 → 5.08 | 5.58 → 4.08 | 0.715 → 0.524 | 7.9 → 5.2 | 214 → 154 |
| 全输出（Chrome WHEP 64、RTMP 64、FLV 64、RTSP 64、HLS 16、GB 64） | 4.83 → 3.62 | 7.34 → 5.05 | 5.64 → 4.18 | 0.716 → 0.534 | 8.7 → 4.4 | 286 → 228 |

全部运行 PASS；UDP 丢包、TCP 重传、SRTP 解保护失败均为 0；每个 WHEP 观众每秒 50 个音频包。

转码器（bpftrace，测量窗口内的快照）：`a` 每源 1 个 AAC→Opus egress（输出 1 声道），并因此需要 Opus→AAC 持续运行；`d` 的 egress 为 0。纯 Chrome WHEP 负载下 `d` 的 Opus→AAC 只在就绪探测阶段（探测用 FLV 读者加入、离开）启动，测量窗口内没有 AAC 消费者、转码器空闲；有 FLV 或其他 AAC 输出时 `d` 仍保持每源 1 个 Opus→AAC。两个版本在窗口内都没有转码器或 egress 重建。

带宽：每个 Chrome WHEP 观众的接收速率 `a` 2128 kbps、`d` 2166 kbps，直通多 38 kbps（源 Opus 的原码率替代 egress 的 64 kbps 单声道，含 RTP/SRTP 开销），视频约占 2.0 Mbps。

### 容量（纯 Chrome WHEP，每源 2 观看者）

| 版本 | 最大持续 | 最忙核 | Gcycles/s | PSS | 下一档 |
| --- | --- | --- | --- | --- | --- |
| a | 256 源 / 512 观众（2/2 PASS） | 0.941–0.946 | 17.4 | 749 MiB | 288 FAIL：8 核饱和，UDP 丢包 281551 |
| d | 1056 源 / 2112 观众（2/2 PASS） | 0.916–0.920 | 19.9 | 2.0 GiB | 1088 FAIL：8 核饱和，UDP 丢包 338178 |

`d` 在 768–960 源时最忙核 0.744–0.849，留余量（最忙核 ≤0.87）约 960 源。容量提升 4.1 倍，与上一份报告 Firefox 类 offer 直通的 1056 源相同；失败点都是 8 核整体饱和后的 UDP 丢包，没有单核瓶颈。

### 真实浏览器

[`d_chrome.py`](d_chrome.py)（作业 `s06` 为 `a`/`d`，`s10` 为加入关键帧触发后的 `d`，以 `s10` 为准；`runs4/d-chrome-a`、`d-chrome-d`、`d-chrome-kf-d`）。三路 WHIP 源：

- `mono`：`whip_publisher`，单声道 64 kbps；
- `lr`：`whip_publisher`，立体声 128 kbps，右声道为左声道 −12 dB（ffmpeg astats：−24.9 / −36.9 dB）；
- `chrome_pub`：真实 Chrome 推流，假摄像头视频固定 640×360 且 `maintain-resolution`（WHIP 拒绝推流中的视频配置变化），WebAudio 每秒一次蜂鸣、右声道 −12 dB。第 30 s 在 Chrome 本地把服务端应答改为 `stereo=1` 重新协商，第 61 s 改回 `stereo=0`。

每路源挂 5 个 `whep_fanout`（`stereo=0`、Chrome 默认、Firefox 默认、`maxaveragebitrate=32000`、`maxplaybackrate=16000`），记录收到的 Opus TOC 立体声标志并回放解码。每路源还挂真实 Chrome 观众，用 WebAudio 声道分离器每 50 ms 测左右 RMS，按右/左比值判断实际播放声道：约 0.25 为立体声，1.0 为单声道上混。`lr` 另加 3 个带 `maxaveragebitrate=32000`、`maxplaybackrate=16000`、`a=maxptime:60` 的真实 Chrome offer。第 45 s 起各加入一个 HTTP-FLV 读者，触发按需 Opus→AAC。结束时 `mono` 推流端 SIGKILL，`lr` 推流端 SIGTERM，Chrome 推流端发 WHIP DELETE。

| 检查 | a（基线） | d（实验） |
| --- | --- | --- |
| Chrome 默认 offer 观看 `mono` | egress，应答 `sprop-stereo=0`；播放右/左 = 1.0 | 直通，`sprop-stereo=1`；播放 1.0；TOC 全单声道 |
| Chrome 默认 offer 观看 `lr` | egress 单声道；播放 1.0 | 直通，TOC 全立体声；**播放仍为 1.0**（Chrome 下混） |
| `stereo=0` offer 观看 `lr` | egress，TOC 全单声道 | 直通，TOC 全立体声（即 SHOULD NOT 的偏离情形） |
| Firefox 类 offer（`stereo=1`）观看 `lr` | 直通立体声 | 直通立体声 |
| `maxaveragebitrate=32000` / `maxplaybackrate=16000` / `a=maxptime:60` | egress，单声道 | egress，单声道，`sprop-stereo=0`（限制未放宽） |
| Chrome 推流端实际编码（经 `stereo=0` 等直通观众的 TOC） | — | 0–30 s 单声道；切到 `stereo=1` 后立体声；切回 `stereo=0` 后单声道 |
| Chrome 观看 `chrome_pub` | 播放 1.0，无隐藏 | 播放 1.0；切换到立体声时 1 次隐藏 240 样本（5 ms），`s06` 中切回单声道时 1 次 120 样本 |
| whep_fanout 回放（`mono`、`lr`） | 无缺口、无警告，A/V −3.3 ms | 同左 |
| FLV 中途加入（按需 Opus→AAC） | 无缺口，A/V −5.3 ms | 无缺口，A/V −3.8 ms |
| 源结束与清理 | DELETE 返回 204；DELETE 与 SIGTERM 的源及其观众 0.1 s 内结束；SIGKILL 的源由 ICE 活动超时在约 25 s 后结束；WHEP 会话 21 建 21 关，receivers 清空，服务端存活 | 同左（receivers 22 s 清空） |

结论：

- Chrome 的 inbound 统计里 `channels` 始终为 2，不能据此判断播放声道。实际播放检查表明：Chrome 在本地 offer 为 `stereo=0` 时把收到的立体声 Opus 下混为单声道播放，直通不会让 Chrome 观众听到立体声。直通对 Chrome 观众的音质影响只有“少一代有损编码”；对立体声源，多出的立体声数据被接收端丢弃，这正是 §7.1 用 SHOULD NOT 防范的网络浪费。
- 真实 Chrome 推流端在服务端缺省应答下发送单声道，`stereo=1` 时才发送立体声，并能在运行中按协商切换。因此 Chrome→Chrome 的直通不发送立体声，不构成偏离；偏离只出现在 FFmpeg、`whip_publisher` 等发立体声的源和 WHEP 拉流源上。
- 声道切换点上，直通观众会看到发送端编码器重配的不连续，最多 1 次 2.5–5 ms 隐藏；基线经 egress 重编码，没有隐藏。

### 决定

| 项 | 结论 |
| --- | --- |
| 协议 | 直通属于多目的地网关，适用 SHOULD NOT。Chrome 等遵守 MUST NOT 的推流端不触发偏离；立体声源会向 `stereo=0` 观众发送被下混丢弃的立体声，每观众约多 38 kbps（总接收速率的 1.8%）。`maxplaybackrate`、`maxaveragebitrate`、`maxptime` 限制保持不变，`sprop-stereo` 按源声道给出 |
| 收益 | 64 源 × 2 Chrome WHEP：cycles −67%，最忙核 0.667→0.214；容量 256→1056 源（4.1 倍）。叠加 FLV 或全协议时 cycles −25%～−26% |
| 代价 | 立体声源下的带宽（如上）；声道切换点最多 1 次 5 ms 隐藏 |
| 建议 | 偏离有明确的协议依据、受限的代价和实测收益，可以作为单独评审的生产改动合入；如不接受立体声源的带宽偏离，保留旧行为。本轮按要求未合入，补丁见 [`d-passthrough-experiment.diff`](d-passthrough-experiment.diff)。合入时需同步 `docs/protocols.md` 中“Chrome 观众走 egress”的描述 |

## 复现

作业脚本在 [`jobs/`](jobs/)，按名称顺序执行；包装脚本 [`run_perf.sh`](run_perf.sh)、[`lib.sh`](lib.sh) 指定运行目录与 `SERVER_BIN`。

| 作业 | 内容 |
| --- | --- |
| s01 | 4 种编码 × 3 种 GB 传输矩阵项 |
| s02、s02b、s02c | GB 客户端与 `gb_ps_demux` 的 ASan/TSan、负向输入、sanitizer 日志正向对照（s02 中 demux 部分的环境变量写法有误未生效，由 s02b 补跑） |
| s04b | 阶段 C 分层时间戳（s03 第一次运行因清理超时与抓包过滤失败作废） |
| s04c | 完整 Release 矩阵 |
| s05 | 阶段 D A/B 性能与容量搜索 |
| s06 | 阶段 D 真实浏览器 a/d（chrome_pub 的 whep_fanout 观众因没有关键帧未收到媒体） |
| s09 | 补跑 `ab-chrome-a-1`（s05 开头与 s07 都被环境检查拦下：前者端口残留，后者磁盘低于 6 GiB） |
| s10 | 阶段 D 真实浏览器 d，加入关键帧触发与 AudioContext resume（以此为准；s08 因磁盘被拦下） |

s04、s04d、s04e、s04g 的浏览器运行因 Chrome 推流端视频配置变化、就绪判断过早、手动终止或环境检查而作废，未保留脚本副本；修正都已并入 [`d_chrome.py`](d_chrome.py)。
