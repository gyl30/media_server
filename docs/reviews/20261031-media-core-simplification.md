# 媒体核心精简审查（2026-10-31）

## 结论

本轮只读审查当前媒体核心，基线为 `aa083a5` 加阶段一工作区改动（Chrome Opus 直通）。

现有媒体架构已经足够精简：
- registry、dispatcher、固定轨道、共享派生输出和网络层的每项状态，都能对应到独立事实或可达时序；
- 没有找到过期的同 `stream_id` 替换、轨道更新、GOP/history 或兼容回退残留；
- 没有找到只声明不调用的接口。

真正值得实施的只有一项 P1，它删除一个从不变化的配置参数，收益很小。另有两项 P2 只在出现真实需求或测量证据时才值得做。**不建议启动下一轮以 LOC 为目标的媒体代码精简。**

| 级别 | 候选 | 一句话 |
| --- | --- | --- |
| P1 | WHIP 接收配置的 Opus 声道数 | 两个调用方都传 2，删除这个配置字段和 WHIP 应答中的同名值 |
| P2 | 纯视频 WHEP 观众挂到 Opus 伴随流 | 避免 video-only WHEP 观众触发无人消费的 Opus→AAC，约 3 行，无新状态 |
| P2 | 空闲时释放 Opus→AAC 转码器状态 | 用转码器自身的资源存在性代替接收器的 `audio_transcoder_active_`，收益取决于每路空闲转码器的内存实测 |
| KEEP | 见下文 9 项 | 状态各自表达独立事实或协议阶段 |
| REJECT | 见下文 7 项 | 只减少表面重复、增加抽象或没有性能证据 |

## 范围与方法

- 源码：`media/core`、`media/codec`、`media/webrtc`、`media/rtsp`、`media/rtmp`、`media/gb28181`、`media/ps`、`media/flv`、`media/hls`、`media/net`，约 2.2 万行。
- 依据：
  - 决策 D01–D13、约束 C01–C08、经验 L01–L10；
  - 三份近期报告：[`20261030-protocol-matrix-performance`](../performance_reports/20261030-protocol-matrix-performance/report.md)、[`20261031-ondemand-opus-aac`](../performance_reports/20261031-ondemand-opus-aac/report.md) 和 [`20261031-gb-ps-rtsp-aac-chrome-opus`](../performance_reports/20261031-gb-ps-rtsp-aac-chrome-opus/report.md)。
- 方法：
  - 列出各 session 和派生对象的成员状态，逐项找写入点、读取点和对应事实；
  - 对头文件声明的约 290 个函数名做全仓引用计数，只用来找候选，不据此下结论；
  - 用调用链核对候选点，必要时读第三方库实现（libmpeg PS 编码时写 PSM 的时机）；
  - 用历史决定排除已裁决的方案。
- 性能依据：
  - 每源最大固定成本是 Opus→AAC（约 23 Mcycles/s）和 AAC→Opus egress（约 20 Mcycles/s）；前者已按需运行，后者在 Chrome 直通后只剩受限 offer 使用；
  - 每观看者成本主要是 SRTP 和逐包 `sendto`，属于协议必需开销；
  - 没有其他热点支撑结构性改动。

## P1：明确可以精简

### P1-1 WHIP 接收配置的 Opus 声道数

| 字段 | 内容 |
| --- | --- |
| 模块 | `media/webrtc/whip_media_receiver.{h,cpp}`（`whip_media_receiver_config::audio_channel_count`）；`media/webrtc/webrtc_sdp.cpp` 的 WHIP 应答（`.audio_channel_count = 2`）；`whip_session.cpp`、`whep_pull_session.cpp` 两个调用方 |
| 当前职责 | 决定 Opus→AAC 转码器的输入/输出声道数、AAC 码率（64 kbps × 声道），以及 AAC 轨道和 Opus 伴随轨道登记的声道数 |
| 发现 | 两个调用方都传 2：WHIP 应答对有音频的情况恒写 2，`whip_session` 读取它并以 `value_or(2)` 兜底；WHEP 拉流直接写 2。RFC 7587 的 rtpmap 固定为 `opus/48000/2`，解码器总能输出立体声，这个值没有协商来源。现在它是一个看似可配、实际恒定的参数，并在 `webrtc_answer` 里多出一个 WHIP 专用的值 |
| 历史决定 | `docs/protocols.md` 已规定“WHIP/WHEP 拉流源的 Opus 轨道一律按 2 声道登记”，与常量一致；没有发现曾把它作为可变参数裁决 |
| 建议 | P1 |
| 最小方案 | 删除配置字段，接收器内用已有常量风格写 `constexpr std::uint16_t opus_channel_count = 2`；删除 WHIP 应答中的 `.audio_channel_count`；两个调用方不再传值。`webrtc_answer::audio_channel_count` 仍由 WHEP 应答使用，保留 |
| 预计影响 | 减少一个配置字段、两处传参和一处应答赋值，约 −6 行；运行时与依赖不变 |
| 风险 | 低。行为不变：WHIP 与 WHEP 拉流的声道仍为 2；涉及 WHIP 应答结构，但没有其他读者 |
| 验证 | 构建；矩阵 `whip-h264-opus`、`whep-pull`、`whip-chrome-offer`、`whep-pull-chrome-offer` 四个场景；日志中转码器为 `opus 48000hz 2ch → aac 2ch` |
| 结论 | 可以实施，但收益小，只值得作为下一次触碰该文件时的顺带改动，不单独立项 |

## P2：有价值但需要验证

### P2-1 纯视频 WHEP 观众挂到 Opus 伴随流

| 字段 | 内容 |
| --- | --- |
| 模块 | `media/webrtc/whep.cpp`（直通选择）、`media/core/media_stream::has_sinks()`、`whip_media_receiver::on_demuxed_packet()` |
| 当前职责 | 有伴随流且 AAC 应答带音频时，尝试用伴随流的 Opus 轨道生成直通应答；否则观众挂在 AAC 主流上 |
| 发现 | `has_sinks()` 统计主流上所有存活 sink，不区分是否消费音频。WHIP/WHEP 拉流源上 offer 只含视频（或音频被拒）的 WHEP 观众挂在主流上，会让 Opus→AAC 一直运行，却没人消费 AAC。同类情形还有 RTSP 只 SETUP 视频的播放；其余主流订阅者（FLV tag、HLS、PS、egress）都消费音频 |
| 历史决定 | 按需转码（`6d9d0e1`）只以“是否有订阅者”判断，报告接受了“只要有 AAC 类观众就省不下来”的代价；没有讨论 video-only 观众 |
| 建议 | P2 |
| 最小方案 | 在 `whep.cpp` 中，伴随流存在且 AAC 应答没有音频时，也把 `source` 设为伴随流：伴随流的视频轨道与主流相同（同一 id、同一 payload），应答无需重算。约 3 行，不新增状态，也不新增订阅管理。RTSP 只 SETUP 视频的情形不处理（见 REJECT R5） |
| 预计影响 | 只有 video-only WHEP 观众时，每源省约 23 Mcycles/s；其他情形不变 |
| 风险 | 低：伴随流随源结束，生命周期与主流相同；`webrtc_packetizer::startup()` 对 payload type 为 −1 的轨道直接 `continue`，已核实伴随流中未协商的 Opus 轨道会被跳过 |
| 验证 | `whep_fanout` 增加只含视频的 offer 后，检查服务端日志中没有 `input opus` 转码器启动，视频解码和清理正常 |
| 结论 | 需要先确认是否真的有 video-only WHEP 客户端（监控墙等）；没有需求时不做 |

### P2-2 空闲时释放 Opus→AAC 转码器状态

| 字段 | 内容 |
| --- | --- |
| 模块 | `media/webrtc/whip_media_receiver.{h,cpp}`（`audio_transcoder_config_`、`audio_transcoder_active_`）、`media/codec/audio_transcoder.{h,cpp}` |
| 当前职责 | 没有 AAC 订阅者时跳过转码，并把 `audio_transcoder_active_` 置假；订阅者重新出现后，用保存的配置重启转码器，并重新对齐时间线 |
| 发现 | 空闲时转码器的 FFmpeg 解码器、libfdk 编码器、重采样器和 FIFO 仍然驻留；“是否在运行”由接收器的 bool 表达，而这一事实本可由转码器自身的 `state_` 是否存在表达（D05：优先用资源成员表达事实） |
| 历史决定 | 按需转码报告接受当前形式；没有讨论空闲内存 |
| 建议 | P2 |
| 最小方案 | 转码器增加 `stop()`（重置 `state_`），接收器用 `output_codec_config().empty()` 或新增的 `running()` 判断是否需要重启，删除 `audio_transcoder_active_`。配置仍须保存，因为重启要用 |
| 预计影响 | 每个只有直通观众或没有观众的 WHIP/WHEP 拉流源，节省一份编解码器上下文的内存；代码量基本持平（删除一个 bool，增加一个方法） |
| 风险 | 低：重启路径已经存在并经过矩阵和容量验证；需确认 `output_codec_config()` 在停止后不再被读取（目前只在 startup 后读取一次） |
| 验证 | 先在 1056 源纯 Chrome WHEP 负载下对比 PSS；收益不足（如每源 <100 KiB）则不实施 |
| 结论 | 先测量，再决定 |

## KEEP：应当保留的关键设计

| 模块 | 保留理由 |
| --- | --- |
| `media_stream` 的 `ended_`、`output_mutex_` 与 PS/FLV 弱引用 | `ended_` 让迟到的 `add_sink` 收到 end（D06/C04）；`ps_output()`/`flv_output()` 会从不同 worker 调用，必须加锁；弱引用让共享输出随最后一个观众释放 |
| `worker_sink_dispatcher` 的分组队列、2500 帧上限和 20 ms 时间片 | 统一跨 worker 分发是 D07 认定的真实共同职责；上限和时间片对应已验证的慢组结束与控制面延迟（`docs/media-lifecycle.md`） |
| `has_sinks()` 逐帧检查分发表 | 在源 worker 上读取，与 `add/remove` 同线程，不需要额外同步或订阅计数；按需转码的状态就是分发表本身，没有第二份事实 |
| `stream_registry` 与 `session_registry` 分离、按对象 identity 删除 | 前者保存有轨道的源，后者保存可停止的会话，两者寿命不同；所有方法都有调用方（媒体侧 receiver/sender、HTTP 控制面、停机） |
| `whep_audio_egress` 的 `viewers_`、`finished_` 和全局弱表 | 关闭订阅回调持有 `self`，析构不能作为释放点，计数决定何时在所属 worker 上 finish；`finished_` 会被其他线程的 acquire 读取，必须是原子量。改成析构驱动，就要在任意 worker 上做清理，违反 D06 |
| `whip_media_receiver` 保存转码配置与 `audio_transcoder_active_` | 重启转码器需要配置；“上一帧之后是否连续转码”是一次跨帧转变，不能由 `has_sinks()` 推导（除非采纳 P2-2，由转码器资源表达） |
| GB receiver 的 topology 与待注册轨道 | PSM 宣告（`announced_topology_`）与码流内 codec config（`video_track_`/`audio_track_`）是不同时刻到达的两件事，先收集再注册固定轨道是 D11 的规定；`video_codec_`/`audio_codec_` 在注册前判断 topology 变化 |
| RTSP 拉流的 RTP-Info 对齐与 RTSP 发布的 SR 对齐分开实现 | 两者分属 PLAY 和 RECORD 阶段，时间线来源不同（D09） |
| 共享输出与各观看者的两层关键帧等待（PS/FLV/HLS 与各 sender/viewer） | 共享层避免为无人能用的帧做封装；观看者层服务于中途加入（D02）。libmpeg 在 IDR 帧必写 PSM，所以 GB sender 从关键帧起播总能拿到 PSM |

另外：
- RTSP 播放的 `track_states_` 保存 SETUP 后的 muxer 索引和通道号，这是协议阶段事实，只顺带缓存了 kind/codec；
- `playing_`、`rtcp_reporting_started_`、WHEP 的 `local_port_` 终态和 worker 的关闭订阅都是 D05、L05、L06 已确认的独立事实，本轮核对后仍然成立。

## REJECT：不值得实施

| 编号 | 方案 | 拒绝理由 |
| --- | --- | --- |
| R1 | 把 RTSP 发布与 WHIP 接收中相同的 SR→pts 映射（约 15 行）抽成共享函数 | 语义相同，但只减少十几行，却要新增跨模块头文件和接口；WHEP 拉流已经复用 WHIP 接收器，没有第三个调用方 |
| R2 | `whep_audio_egress::transcoders_` 由 map 改为单个转码器 | 源最多一路 AAC 轨道，但 map 不维护额外事实，改动只是表面简化 |
| R3 | 删除 `mpeg_ps_output` 的首关键帧等待，交给各 sender | 会为第一个 GOP 前的帧做无用封装和分发，状态数量不变 |
| R4 | 伴随流与主流各自做一次 Annex-B→长度前缀转换，改为在接收器内预先生成 | 只有两条流上同时有 RTP 打包观众时才重复，没有 profiling 证据；本轮不做微小性能优化 |
| R5 | 让 RTSP 只 SETUP 视频的观众不计入 AAC 需求 | 要么新增按轨道的订阅事实（违反“不新增订阅管理框架”），要么让 RTSP 改从伴随流播放（对外 codec 从 AAC 变为 Opus，属于行为变更） |
| R6 | 合并 RTSP 拉流/发布媒体、统一 WHIP/WHEP transport、恢复 reader/GOP | 约束 C01、C03 与决策 D07、D09 已裁决；本轮没有新证据 |
| R7 | 删除 GB receiver `update_track_from_packet()` 中 codec 不同则返回的分支 | 在上游 codec 检查后看起来不可达，但只省 4 行，而且要先证明两套 codec 映射完全一致；不值得 |

## 已经没有必要继续研究的方向

- 媒体核心分发：分组、快照和队列已统一，按需转码依赖的分发表也是唯一事实。
- 网络与 worker：transport、取消、关闭订阅、端口池和 idle timer 都没有冗余状态或无调用接口；按要求不以替换网络模型为目标。
- 每观看者的协议发送成本（SRTP、逐包 `sendto`、TCP 写）：属于协议必需开销。
- 音频转码器内部实现：复杂度来自 FFmpeg/libfdk 的时间线与 FIFO 处理，没有 profiling 证据支持重写。
- 输出层的再次共享：FLV tag、PS、AAC→Opus 已按源共享；RTMP chunk、RTP 序号、SSRC、SRTP 必须按观看者独立。

## 是否启动下一轮媒体代码精简

不建议。P1-1 可以作为下次修改 WebRTC 接收代码时的顺带改动；P2-1 等到出现 video-only WHEP 需求时再做；P2-2 先做一次内存测量。除此之外，再精简只会触及已有独立职责的状态，或者违反已裁决的边界。
