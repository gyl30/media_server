# 已接受的架构决策

以下决定以 `813b4f1` 源码、可核对提交和已跟踪报告为依据，核心决定及原因直接写在本文。已备份的旧个人 `AGENTS.md`、未跟踪的本地 `docs/project_decisions.md` 仅补充历史讨论，缺少它们不影响理解这些决定。普通编码规范另见已跟踪的 [构建与维护约定](../docs/build-deploy.md)。

## D01：固定轨道的统一内部媒体

- 背景：多个输入和输出协议需要共用媒体，同时曾存在运行时轨道更新、reader 和历史缓存。
- 决定：输入协议整理完整 tracks 后注册 `media_stream`；一个对象代表一个 source generation。内部传递 canonical frame，配置变化通过结束旧代、重新建立新代表示；重新发布由外层业务负责，不在同一对象更新轨道。
- 原因：协议报文和网络状态留在边界，输出读取固定轨道即可初始化；代际切换替代轨道事件、cursor/resync 和多份版本状态。
- 相关历史：`bd99e20`「固定媒体流轨道并删除运行时轨道更新」、`4c2a1ab`「删除媒体历史并直接推送到 sink」、`1770d0b`「收口 sink 推送终止生命周期」。现行依据：`media/core/media_types.h`、`media_stream.*`、[媒体核心边界](../docs/architecture.md)。

## D02：源主导推送与自然关键帧

- 背景：后加入的输出需要可解码起点，但反向控制会把 viewer 的需求传播到输入源；历史模型还包含 GOP/history。
- 决定：输出等待实际到达的自然关键帧，不回放历史，不反向要求源产出关键帧，不恢复已淘汰的上游控制机制；GB SIP 点播继续由业务信令负责。
- 原因：保持 live push 的单向职责，不为每个观看者维护历史或增加源控制状态。首媒体等待受源 GOP 影响，是此模型的真实代价。
- 相关历史：`4c2a1ab`、`f54e597`「统一播放会话关键帧起点」、`d84e0b4`「收口播放会话关键帧等待状态」。历史「系统验证」（`git show 3c3167e:docs/system_validation.md`）保留了 WHIP 无下一关键帧失败及测试端显式 resume 的覆盖边界，后者不是服务端 PLI/FIR；本地 `docs/project_decisions.md` §4 仅补充讨论背景。

## D03：按源共享派生媒体，保留 viewer 身份

- 背景：多个 WHEP viewer 重复 AAC→Opus 转码，多个 GB sender 重复 PS 封装会增加同源固定成本。
- 决定：AAC→Opus 按源代及音频参数共享，MPEG-PS 按源代共享；viewer/sender 的 RTP、SSRC、sequence、DTLS/SRTP 和网络状态仍独立。
- 原因：复用相同的媒体计算，不混合不同连接的加密密钥、序列和生命周期。HLS 的片段 retention 是独立业务需求，不是被删除的通用媒体历史。
- 相关历史：`cae4010`「按源代共享 GB28181 MPEG-PS 封装」、`80b5295`「接入 MPEG-PS 统一分发」；当前 `whep_audio_egress.cpp`、`media_stream::ps_output()`、`media/ps/`。

## D04：网络所有权下沉，错误策略上移

- 背景：多个 session 曾分别维护写队列、buffer 和发送调度，网络错误也曾通过重复状态或关闭桥接传播。
- 决定：TCP/UDP transport 负责 socket、串行写入、buffer lifetime、completion 和错误报告；session 决定是否结束。同步错误直接返回，transport 不 retry/reconnect。
- 原因：序列化是网络职责；恢复和生命周期依赖协议。把两者合并会隐藏失败、复制终态并扩大底层接口。
- 相关历史：`9b77f94`「网络 I/O 错误改为上报 owner」、`a1f2f20`「明确 TCP 错误回调所有权移交」、`e4187c3`「精简 UDP 传输终止状态与错误传播」；当前 `media/net/{tcp,udp}_transport.*`。UDP 每项保留完整 datagram 和 endpoint，overflow 返回入队拒绝并丢新包；它与 TCP 背压不同。`service.cpp` 的固定 3 秒 accept 重试是上层监听策略，不是 transport 自动恢复。

## D05：减少重复 lifecycle state，保留可达时序

- 背景：`started_`、录制/发布 bool、关闭标志曾重复表达资源事实，但异步取消和不同协议阶段确实需要区分。
- 决定：状态必须表达独立事实；优先使用资源成员、固定 tracks 和对象 identity。一次启动不额外防重；有同步失败时保留错误返回。
- 原因：重复真相需要额外同步，容易在回调与清理之间失配。按名称批量删 bool 同样错误：RTSP `playing_`、GB `rtcp_reporting_started_`、worker 取消和 registry 停止保护有真实用途。
- 相关历史：`ecb03b7`、`0c9de93`、`3d4b1c8`；详见 [lessons.md](lessons.md)。`16ec823` 已删除 WHEP 的历史 `shutdown_requested_`，当前以 owner 上的资源事实守住终态，不能照旧审计恢复这个字段。

## D06：在所属 worker 清理，并主动断开回调所有权

- 背景：关闭请求、异步完成及跨 worker sink snapshot 可以交错；移出 registry 或 remove sink 不会撤销已持有的回调。
- 决定：session 的 `shutdown()` 投递到 owner，由自身 `safe_shutdown()` 清理；父调用子 `shutdown()`。必要回调保活，清理时取消任务、清空回调并释放资源。
- 原因：把资源操作放在同一执行边界，既避免悬挂访问，也避免强引用环长期保留对象。网络在途 buffer 要独立活到 completion，不能依赖 queue 未被清空。
- 相关历史：`17c1904`「统一会话关闭为投递到自身 io 执行清理」、`d8a2428`、`afd2cc3`、`f441a46`；当前 `worker_context.*`、`session_registry.*`、相关生命周期测试。worker shutdown subscription 与停止登记保护是后续可达停机问题所需，不按早期“直接 stop worker”概述删除。

## D07：只统一真实相同的基础设施

- 背景：媒体流与 PS 输出曾复制跨 worker 分发；WHIP/WHEP 也有相似的协议流程。
- 决定：统一 worker-group dispatch 和通用网络队列，保留 WHIP/WHEP 各自的协议/媒体生命周期；当前不新增共同 `webrtc_transport`。
- 原因：前者确实消除重复调度算法；后者已有 UDP/STUN/DTLS/SRTP 组件，媒体方向与关闭归属不同，再抽总类需要新的回调与协调，收益不足。
- 相关历史：`68c45e0`「统一跨工作线程 Sink 分发并接入媒体流」、`acee8da`「记录 WebRTC 传输只读设计审计」、`9200b00`「记录基础设施重复设计审计」。两份旧审计从对应提交的 Git 文件树追溯，其端口池、启动阶段和字段细节不是当前源码规范。

## D08：WebRTC 阶段与源结束分别表达

- 背景：SDP 接受、ICE nomination、DTLS、SRTP ready 与媒体开始是不同事件。
- 决定：维持现有一次性 SDP、ICE-Lite、DTLS/SRTP 模型，不新增 Trickle ICE、ICE restart、自动重连或反向源控制。合法、认证后的再次 nomination 可更新 endpoint；普通 check 不改变媒体目标。
- 原因：保持已经验证的最小协商范围，避免把传输建立等同媒体 ready，或把 viewer 失败等同 source 结束。
- 相关历史：`cc63c0d`「修复 WebRTC 候选地址切换后的媒体传输」、`3dc1603`、`5023b2c` 的创建/启动职责整理；当前 `whep.cpp`、`whip.cpp`、两个 session 和历史「系统验证」（`git show 3c3167e:docs/system_validation.md`）。

## D09：RTSP 的协议阶段不合并

- 背景：DESCRIBE/SETUP/PLAY 和 ANNOUNCE/SETUP/RECORD 有真实控制顺序，TCP/UDP 资源准备也不同。
- 决定：保留 session identity、已 SETUP 轨道与 PLAY 阶段；成功 PLAY 回复先于媒体订阅。Pull 的 URL 在入口解析，恢复异步 resolve/connect 后校验取消与 registry identity。
- 原因：持有 source 不代表已开始播放；迟到完成不能让旧接收器替换新代或访问已释放媒体。
- 相关历史：`7ca4a9b`、`3af9496`、`a753d54`；当前 `rtsp_play_session::on_play()`、`rtsp_pull_session::run()`。V2 不可达结论仅适用于当前单线程 worker 和已核对的 Boost 1.92 调度，见历史「专项报告」（`git show 3c3167e:docs/reviews/rtsp_play_v2_lifecycle_review.md`）。

## D10：RTMP 初始化媒体服从 Play.Start 顺序

- 背景：muxer 在注册轨道时立即生成 codec config，而 RTMP 要先发 Play.Start。
- 决定：保留 prepare 阶段的 `initial_packets_`，Play.Start 成功后再 startup 输出初始化包并订阅媒体；输入元数据和初始音视频配置用于收集完整固定 tracks。
- 原因：这些成员表达真实协议阶段，不能只为统一 startup 形式删除。Publish/Play 仍由连接的角色对象区分。
- 相关历史：`f0f3099`「收口 RTMP 播放授权后初始化时序」、`0c9de93`；当前 `rtmp_session.cpp` 的 `rtmp_server_start()` 与 `rtmp_play_session::{prepare,startup}`。

## D11：GB28181 区分业务身份、PS topology 与传输

- 背景：device、registration、channel、live 和 RTP 会话寿命不同；PSM 宣告与 codec 配置就绪也不同步。
- 决定：Go 管理 SIP/live，媒体侧管理 RTP/PS。同 device/channel 可共享 live；关闭 viewer 或 viewer 传输失败不停止上游，停止 live 则结束该路所有观看者。先收集并确认 topology/config，再注册固定轨道源；UDP、TCP active/passive 保留实际建连差异。RTCP 开始状态不从 timer 猜测。
- 原因：合并这些事实会丢失代际 fencing 或协议准备阶段。共享 PS 不共享各 sender 的 RTP 身份；不支持的音频降级也不能伪装成完整双轨成功。
- 相关历史：`b49a45b`、`971a803`、`f3b2966`、`72be980`；共享 live/viewer 的范围见 `f6b2680` 的 Web UI 验证记录。当前依据：`gb28181_rtp_receiver.*`、`gb28181_udp_sender_session.*`、`signaling/live_session.go`、`live_session_test.go` 和 `signaling/web/whep.js`。

## D12：资源认领与废弃路径保持单一权威

- 背景：旧控制面曾有 registration/heartbeat/allocation/claim；HLS 还曾在认领授权前创建分片器。
- 决定：使用当前统一一次性信令授权，不保留旧控制 API/路由兼容层。HLS 先 verify，再共享 segmenter 和创建独立 play session；授权后初始化失败不增加 token 恢复流程。端口池仅管理 P 的 acquire/release，bind 失败直接结束，不扫描重试候选。
- 原因：兼容和两阶段认领增加额外事实来源；授权不能只保证响应正确而允许未授权创建处理资源。端口冲突按已接受的部署隔离契约处理，不由池隐藏。
- 相关历史：`c2eb9fe`、`0a8f3f9`、`2bde22f`、`f1a6d49`、`07cf46e`、`f0c61cd`；当前 `signaling/stream_token.go`、`hls_http_session.cpp`、`media_port_pool.*`。

## D13：已接受的控制边界不是自动修复清单

- 背景：历史审查曾把异步提交、空闲刷新及日志等既定行为反复当作缺陷。
- 决定：停止推流后旧输入可能迟到登记的竞态只记录；GB UDP 收到数据即刷新空闲；SSRC 更新和 receiver 删除的 204 表示异步提交，不等待执行/释放；完整一次性 token/URL 的 INFO 日志沿用当前既有行为。
- 原因：需求方已接受这些控制语义，不为它们追加 pending/closing、跨 worker 完成通知或同步 HTTP 等待。完整 token/URL 日志只是这一已有实现的历史取舍，不代表所有部署条件下都安全，也不构成扩大敏感信息日志或公开传播凭据的通用许可；新增日志或部署需求应单独评估。新的需求或真实新证据才能重新开启相应取舍。
- 相关历史：历史「HLS 与复杂度审计」（`git show 3c3167e:docs/hls_auth_complexity_audit.md`）记录了范围约束，另有 `caacd25`、`3e99476`「撤销接收会话关闭等待机制」；当前 GB receiver、HTTP 控制路径及 `signaling/stream_token.go` 日志。
