# 媒体核心边界

## 从产品能力推导的模型

一个发布实例需要确定的轨道配置、带时间戳的媒体帧、起播历史和结束通知。多个播放者需要独立的消费节奏，但不应让发布者等待慢播放者。因此核心只有两种交付关系：源 worker 上的同步派生处理，以及消费者 worker 上的有界异步订阅。它们是不同的调度契约，不需要统一成带 mode 的接口。

```text
输入协议 → 归一化 → media_stream（一个发布实例）
                         ├─ 有界 GOP history → media_reader → 输出协议 session
                         └─ media_sink → 确定性派生处理 → 可选的派生 history
                                                        └─ media_reader → 输出协议 session
```

`media_stream` 的对象身份就是 source generation。同名重新发布必须创建新对象；名称只用于发现，不能作为派生处理或订阅状态的代际身份。canonical 视频为完整 H264/H265 Annex-B AU 或已支持的 AV1 temporal unit，音频为 AAC/ADTS、Opus、G711A/U。核心不保存 RTP、FLV、PS、ICE 等协议状态。

轨道和 `config_version` 由源 worker 修改。不可变轨道快照是跨 worker 的发布视图，不是第二份可独立修改的轨道配置。历史条目同时保存帧和发布时的配置版本；订阅者不能用当前轨道配置冒充历史帧的配置。有限 GOP history 属于发布实例，消费者只有游标和至多一个未完成读取；落后的消费者从可解码的历史位置重新同步。

`media_reader` 按消费者的节奏请求下一项。它持有游标、批次和等待状态，输出 session 只决定何时继续读取，并持有自己的封装、加密、背压和 socket。reader 回调在消费者 worker；源的发布、历史和同步 sink 在源 worker。两种 worker 之间只搬运共享的媒体引用和有界批次，不为每帧向每个消费者无限投递任务。

`media_sink` 只用于确定性、可共享且适合在源 worker 同步执行的处理。MPEG-PS 输出同时是 sink 和派生 history：一次封装，多名 GB 发送者各自维护 RTP 与传输状态。HLS 分段也是 sink，但播放列表与片段在源结束后仍需保留，因此有独立的保留生命周期。WHEP 的 AAC→Opus 处理成本较高，位于共享处理 worker，生成新的 `media_stream`；不为接口统一而强迫它在源 worker 同步运行。

媒体源目录只负责按名称发现当前发布实例，并以对象身份保护删除。由 HTTP 控制接口创建、按 `stream_id` 删除的 GB/RTSP 会话属于控制层；它们可以早于媒体源存在，也可以在媒体源消失后继续完成关闭，不能作为媒体源目录的状态。协议输入在归一化为上述轨道和帧时结束；协议输出从 session 的 mux、packetizer、控制状态和传输开始。

## 不采用的结构

- 不用 publisher-driven 的逐帧、逐消费者 `post`：慢消费者会积累无界队列。
- 不把同步 sink 和异步 reader 合成带模式的万能 consumer：发布路径与慢消费者隔离的契约会变得隐含。
- 不建立通用事件日志或 MediaGraph：现有产品只需有界起播历史，额外的事件种类、保留策略和节点调度会增加核心词汇。
- 不照搬 mms-server 的协议桥接矩阵、ZLMediaKit 的全协议 muxer 或 SRS 的逐消费者媒体队列；只借鉴它们的 source→consumer、按需派生和有界起播缓存。

在这个模型下，`media_history` 的模板与批次是内部实现，不是协议层必须理解的一级领域概念。协议特定的转换器和保留输出仍可有独立对象，因为它们确实拥有不同的资源或生命周期。只有出现新的真实共享需求时，才考虑比直接 source ownership 更通用的派生输出管理方式。
