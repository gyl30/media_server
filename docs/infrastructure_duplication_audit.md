# 基础设施重复设计只读审计

BASE_HEAD: `30c09f4394c47c8155d9b52606ec5554a6e09e7c`

首次审计基线：`acee8da2481924618955afeb4d8538e4d72c5385`；完整报告已由 `9200b00` 提交。本次复核远端更新，保留原有候选分析，不重复冻结的 WebRTC 协议审计。

结论：**没有达到“值得实现”门槛的新候选，当前架构级主动精简基本完成。** 当前 transport、fanout、registry、共享派生输出的边界合理；未发现第二套需要下沉的通用 socket writer、worker-group drain 或 reservation rollback。没有确认的 correctness finding，也没有明显过度抽象。

发现四处调用 avpkt2bs 的小型 handle/payload glue，但 framing/realloc 算法已经集中在依赖中；进一步封装净收益很小。不能把它归类为与过去 TCP/UDP queue 或 worker fanout 相当的完整基础设施重复。

本次只读源码、调用链、历史和测试断言；只更新本文件及对应 JSON。**没有执行构建、CTest、ASan/UBSan、真实协议套件或性能测试。** 下文历史验证仅引用冻结文档，不作为本轮运行结果。没有修改生产、测试、CMake 或修复疑似 bug。

## 基线、历史与范围

首次审计从干净的 acee8da 开始，完整阅读最近 50 个提交。本次执行 fetch、status、HEAD/远端和最近 60 个提交检查，实际远端已更新到 30c09f4，完整列表在 JSON 的 history_reviewed 中。

主工作区 HEAD 等于 origin/main，但已有 bench/codec_interop_verify.py、bench/lifecycle_verify.py 的未提交改动及未跟踪 bench/browser_webrtc_verify.py。未覆盖、回退、暂存或提交这些改动。本次在 `/tmp/media_server_infrastructure_audit-30c09f4` 的干净 detached worktree 中审计：开始时 HEAD 等于 origin/main，status 为空。原工作区的 clean 前置条件未满足，以隔离工作区建立权威源码基线；不能把隔离工作区干净表述为原工作区干净。

主线程完整读取 AGENTS.md、stability_verification.md、udp_transport_verification.md、worker_sink_dispatcher_verification.md、webrtc_transport_audit.md、performance_baseline.md；沿用 codebase-design 的接口、ownership 与净收益判断。首次审计的五个单轮只读检索分别覆盖 HTTP/HLS、core ownership、codec glue、协议残留和测试保护，主线程审查 net/service/dispatcher，并抽查影响结论的 key paths。本次主线程重核 transport、dispatcher 及唯一生产增量；一个单轮只读代理独立核验增量与测试元数据。代理结果经过 CMake 和实际 git 对象核对，不直接当作结论。

冻结历史：98f2e23/179f181/f569568/8a08234 收口 TCP；52f0c95/1bd5b80/43440c8/f572de3 收口 UDP；9abc847/68c45e0/80b5295/d2f91f6/90cdd7f 完成 fanout 与验证；acee8da 记录 WebRTC 不提取结论。还恢复之前 HLS/HTTP/RTMP/GB/DTLS 时序精简与共享 source/派生输出的稳定性和性能输入。没有重新设计这些业务架构。

审计范围：media/core、media/net、media/codec、media/flv、media/ps、media/hls、media/http、service.cpp、service.h、media/webrtc（冻结审计结论及必要 glue/ownership 调用链）、media/rtsp、media/rtmp、media/gb28181、tests、bench、CMakeLists.txt（只读）、third/ireader（相关 API 实现只读核对）。不审第三方全部实现，仅核对相关依赖 API。没有必要通过外部参考项目恢复缓存/reader 或增加新架构。

七类搜索模式与后续核对：

- **containers**：deque/queue/map/unordered_map：网络层写队列、HTTP-FLV chunk、HLS retention、registry 和派生实例 key；继续核对真实职责，而非按容器名称认定重复。
- **async_operations**：post/dispatch/async_wait/async_write/async_send/spawn：核对 owner executor、提交粒度、completion 和取消路径。
- **synchronization**：mutex/scoped_lock/lock_guard/atomic：核对 owner-worker 串行化、group 双线程同步、registry 锁和业务 terminal fencing。
- **termination**：shutdown/safe_shutdown/finish/cleanup/release/remove：核对资源移交、rollback、identity fencing 和队列清理。
- **ownership**：weak_ptr/shared_ptr/enable_shared_from_this：核对 registry、sink snapshot、pending buffer 和 handler 捕获。
- **timers**：steady_timer：核对 arm/cancel/reschedule、业务条件、worker 和 completion 链接。
- **state**：pending_/queued_/drain_/writing_/stopped_/ended_/finished_/started_：核对事实来源；没有仅因字段同名列候选。

搜索只是入口；结论基于 caller、thread/worker、ownership、失败/终止路径、buffer 与 callback lifetime 及测试断言。单次 post、协议 parser 残包、mux packet、RTCP 生成和普通 map/timer 不构成新的基础设施候选。

## 当前基线增量复核

acee8da 至 30c09f4 的生产增量只有 `media/flv/flv_muxer.cpp:51` 的四行 H265 Enhanced RTMP 调用。另有 CMake 增加 `rtmp_hevc_decode`、原 FFmpeg 测试扩展 H265 参数及新增独立 codec 互操作工具。这些是已提交的协议互操作修复和测试，发生在本次只读复核之前，不是本审计实施的修改。

- transport、dispatcher、registry、port pool、worker、共享派生输出实现与首次审计基线逐文件一致。生产增量未增加 queue、mutex、post、completion、terminal state 或资源 ownership。
- 重新执行七类搜索，按文件和匹配内容比较、忽略变动行号，内容均一致：containers 27 行/21 文件，async operations 98/43，synchronization 66/19，termination 523/86，ownership 172/57，timers 23/10，state 90/19。搜索计数仅证明入口未变化，否定性结论同时依赖完整生产 diff 和原调用链分析。
- `async_send_to` 仍只在 udp_transport 内；`drain_queued`、`pending_end`、`max_pending_frames`、`snapshot_sinks` 仍只在 worker_sink_dispatcher 内。HTTP-FLV 的应用 chunk queue 和 HLS retention 不归并到 socket writer。
- H265 互操作修复没有引入新的基础设施算法，也没有提供推翻 WebRTC 不提取结论的新证据。K1–K12、F1–F2 的职责、收益估算和分类保持原结论。
- 本次复核未新启动构建或协议测试。当前新增测试的注册/断言只能说明保护范围，不能据此声称运行通过。

## 已完成且边界合理的基础设施

- **tcp_transport**：socket、串行 queue、byte accounting 和 handler 持有 buffer；一个 write callback 通知协议层。TCP 的 1 MiB 判断在成功 completion 后检查剩余 backlog，属于既有高水位策略，并非 UDP 式入队硬上限。不借本次审计改变该行为。 证据：`media/net/tcp_transport.h:22`、`media/net/tcp_transport.cpp:56`、`media/net/tcp_transport.cpp:72`、`media/net/tcp_transport.cpp:80`、`media/net/tcp_transport.cpp:111`。
- **udp_transport**：owner-executor 调用；每项 datagram+endpoint，1 MiB 入队拒绝 newest；一个 completion callback、bool 返回拒绝结果。handler 独立持有 in-flight datagram；shutdown/重新 bind 的旧 completion 由 stopped/front identity 隔离。无 ICE/RTP/session policy 泄漏。 证据：`media/net/udp_transport.h:37`、`media/net/udp_transport.cpp:75`、`media/net/udp_transport.cpp:96`、`media/net/udp_transport.cpp:104`、`media/net/udp_transport.cpp:139`。
- **worker_sink_dispatcher**：两个真实类型实例，编译期成员函数绑定、0 个新增 runtime callback；同 worker inline，跨 worker 每 group 单 drain、2500 pending、overflow 清 pending/end/detach、正常 end 先 frames 后 END。weak sink、shared snapshot 与独立 group lifetime 保留。业务 owner 仍检查终态和序列化入口；dispatcher 没有第二个业务 ended 状态。 证据：`media/core/worker_sink_dispatcher.h:21`、`media/core/worker_sink_dispatcher.h:69`、`media/core/worker_sink_dispatcher.h:129`、`media/core/worker_sink_dispatcher.h:193`、`media/core/media_stream.cpp:39`、`media/ps/mpeg_ps_output.cpp:137`。
- **stream/session registry 与 source generation**：stream 的对象 identity fencing 与 session 的 stream_id take/object identity remove 均集中在各自 registry。session shutdown_all 先锁内 detach、再锁外调用 shutdown；协议层不复制 map ownership。两者 key/槽位/终止策略不同，边界合理。 证据：`media/core/stream_registry.cpp:23`、`media/core/session_registry.cpp:30`、`media/core/session_registry.cpp:76`、`media/core/session_registry.cpp:123`。
- **共享 MPEG-PS / WHEP AAC→Opus**：PS 每 source 懒取得、weak cache；音频按 source identity+settings 共享、viewer lease 管结束。mux/转码各一份，generation 不按同名跨代复用，未恢复 history/reader/GOP replay。 证据：`media/core/media_stream.cpp:93`、`media/ps/mpeg_ps_output.cpp:167`、`media/webrtc/whep_audio_egress.cpp:143`、`media/webrtc/whep_audio_egress.cpp:169`。
- **media_port_pool / worker_context / tcp_listener / service accept**：端口 acquire/bind rollback 唯一实现；worker 管协程取消与 shutdown subscription；listener 管 bind/listen rollback；service 的三种长期 TCP accept 使用一个编译期 helper。没有为适配这些边界新增 per-session scheduling 状态。 证据：`media/net/media_port_pool.cpp:99`、`media/net/media_port_pool.cpp:135`、`media/net/worker_context.cpp:66`、`media/net/tcp_listener.cpp:15`、`service.cpp:29`。
- **audio_transcoder / codec_utils**：FFmpeg codec/frame/packet/resampler/FIFO 生命周期已有唯一实现，WHIP/WHEP 只配置方向及业务输出；通用时间单位转换已有 helper，RTMP wrap 单独保留协议状态。 证据：`media/codec/audio_transcoder.cpp:74`、`media/codec/audio_transcoder.cpp:114`、`media/codec/codec_utils.cpp:184`、`media/rtmp/rtmp_timestamp.h:16`。

TCP/UDP 各只有一个 write_callback 类型（每对象一个注册回调），不要求两种 transport 在 admission/thread/reentrancy 细节上变成同一个算法。dispatcher 通过编译期 Deliver 绑定而非 std::function，未给调用方增加第二份 queue/terminal 事实。media_stream 的 ended_ 与 PS 的 source_/muxer_ 都是原业务事实；没有“为了适配抽象”新增通用 writer 状态。

## A. 真正值得下一步实现

**0 个。** 不建议继续主动架构重构。以下 B/C 项都是经过排除或延期的评估项，不是待实现任务清单。

## B. 看起来重复但应该保留

成本和代码量均为静态估计，未编写 prototype；负值表示净删行，正值表示净增行。现有 callback/virtual 不算新增。

### K1: TCP/UDP 再合并 generic async write queue

- **1. 当前重复位置**：`media/net/tcp_transport.h:38`、`media/net/tcp_transport.cpp:56`、`media/net/tcp_transport.cpp:80`、`media/net/udp_transport.h:45`、`media/net/udp_transport.cpp:75`、`media/net/udp_transport.cpp:104`
- **2. 当前职责**：queue/buffer ownership、byte accounting、一个 in-flight operation、completion、terminal fencing、错误上报。
- **3. 是否同一 primitive**：部分：职责同属 transport，但提交、admission 和 completion 算法并不相同。
- **4. 独立业务差异**：TCP byte stream 使用 async_write，入口 post，剩余 backlog 高水位上报错误并停止链；UDP 一个 item 一次 async_send_to、每包 endpoint、owner 上同步拒绝新包、保留既有队列。UDP callback 重入 shutdown/rebind 要 front identity；TCP shutdown 本身 post。
- **5. 假设提取的最小 API**：仅假设 enqueue(item)/stop；要保留两套行为仍需 send(item, completion)、admit、完成后推进/错误终止规则，无法由 enqueue/stop 自足表达。
- **6. 新增抽象成本**：callbacks=约 2–3（send/completion/admission；编译期 policy 可替代但不能消除语义成本）；virtual=0；config=0–1（高水位表达）；policy=至少 2（stream/datagram 与 overflow/completion）；new_state=0–1（抽象内终态；两侧 queue/bytes/endpoint 事实仍需持有）
- **7. 可删除内容/净代码**：可搬移两侧 queue/bytes/stopped 和 start/completion/clear 方法，但 session 已无这些状态；共用部分约 40–70 行，适配加组件约 40–90 行，预计净变化约 -30 至 +50 行，未做 prototype。
- **8. Correctness 风险**：owner executor 契约、callback 重入、shutdown 后 buffer、旧 completion、datagram boundary/endpoint、overflow policy 被强行统一。
- **9. 测试保护**：tests/udp_transport.cpp:68/127/176/230 直接覆盖 UDP；TCP 没有同等写队列专项，bench/lifecycle_verify.py:208/330 是协议级间接保护。若未来确有需求，先补 TCP admission/取消/在途 lifetime 的公共边界测试。
- **10. 最终结论**：保持现状

### K2: HTTP-FLV output queue 与 transport queue / HLS response buffer

- **1. 当前重复位置**：`media/http/http_flv_session.h:57`、`media/http/http_flv_session.cpp:168`、`media/http/http_flv_session.cpp:190`、`media/http/http_flv_session.cpp:274`、`media/http/hls_http_session.cpp:238`
- **2. 当前职责**：FLV 应用输出的 HTTP chunk serialization、4 MiB admission、yield writer 持有当前 front、自持有 session 至完成；HLS 是一次 HTTP response body lifetime。
- **3. 是否同一 primitive**：否：FLV 队列是 application/chunk queue，HLS 单次 shared body 不是 writer/drain 算法；它们不是被遗留在 session 的 TCP 通用 queue。
- **4. 独立业务差异**：FLV make_chunk 有 HTTP 编码边界，overflow 关闭会话；transport 不知道 chunk/FLV。HLS 每次 response 捕获不可变 body，不维护 pending writer。
- **5. 假设提取的最小 API**：没有共同的小 API；generic enqueue 要另加 chunk 构造、Beast stream、admission 与 writer 结果适配；HLS 无 enqueue 需求。
- **6. 新增抽象成本**：callbacks=至少 2（chunk/send、error/lifecycle）；virtual=0；config=至少 1（4 MiB 与不同 limit）；policy=至少 2（chunk 与 overflow）；new_state=0–1（writer/terminal；应用 queue 仍不能消失）
- **7. 可删除内容/净代码**：候选 queue 字段 2、方法 2 只能搬家；通用化可能净增约 20–40 行，单次 shared body 捕获 helper 净删不足 10 行。
- **8. Correctness 风险**：shutdown 时不能提前清引用 front 的队列；当前 socket close 后 writer completion 才清 queue。改变这一点会损坏 buffer lifetime。还需保留 chunk 边界和终止。
- **9. 测试保护**：bench/http_flv_fanout.py:30 和 bench/lifecycle_verify.py:208/330 覆盖真实播放/断开；缺少 chunk 精确边界、慢 reader/在途关闭专项。它们不支持把应用 queue 强塞进 transport。
- **10. 最终结论**：保持现状

### K3: HLS segment deque 与 ended segmenter retention

- **1. 当前重复位置**：`media/hls/hls_segmenter.h:43`、`media/hls/hls_segmenter.cpp:226`、`media/hls/hls.cpp:19`、`media/hls/hls.cpp:37`、`media/http/hls_http_session.cpp:238`
- **2. 当前职责**：已完成 segment 的共享 payload、sequence/window 驱逐；全局派生 segmenter 的 ended wall-clock retention 和 generation identity。不是发送调度 queue。
- **3. 是否同一 primitive**：否：6 segment 滑动窗口与 ended 后 12 秒 registry 保留不是同一 eviction/ownership 算法。
- **4. 独立业务差异**：window 随新 segment 完成推进，registry 每秒清理或请求时清理；segment HTTP body 能被独立强持有。TS 16 MiB 失败/重建属于 muxer policy。
- **5. 假设提取的最小 API**：假设 evict(predicate) 只封装容器 erase，不能统一 sequence、结束时间和强引用 payload；没有必要新增。
- **6. 新增抽象成本**：callbacks=至少 1（eviction predicate）；virtual=0；config=至少 2（sequence window/时间）；policy=至少 2（segment/ended registry）；new_state=0（现有 sequence/time/source 都必须保留）
- **7. 可删除内容/净代码**：可搬动约 10–20 行循环；预计净变化接近 0，字段和 source/segment lifetime 均不减少。
- **8. Correctness 风险**：ENDLIST、旧代保留、HTTP body lifetime、muxer ENOBUFS 与 source retention 不能混在通用队列里。
- **9. 测试保护**：tests/media_lifecycle.cpp:177 覆盖代际派生隔离，bench/lifecycle_verify.py:232/266 覆盖旧 ENDLIST 和过期 lease；没有逐 segment window/ENOBUFS/body 在途专项。
- **10. 最终结论**：保持现状

### K4: stream/session registry 与 sender/receiver 槽位泛化

- **1. 当前重复位置**：`media/core/stream_registry.cpp:12`、`media/core/stream_registry.cpp:23`、`media/core/session_registry.h:22`、`media/core/session_registry.cpp:14`、`media/core/session_registry.cpp:76`、`media/core/session_registry.cpp:123`
- **2. 当前职责**：强 ownership、锁内原子 add/find/take/remove、身份 fencing；session registry 还负责停止时 detach 与锁外 shutdown。
- **3. 是否同一 primitive**：部分：都管理 ownership，但 stream 单 key current generation 与 receiver 单槽/sender 多槽/token take 不同。
- **4. 独立业务差异**：stream remove 用对象身份；session 控制请求 take 用 stream_id，cleanup 用对象身份，有 stopping 拒绝。receiver 唯一与 sender-id map 不可机械套同一个槽位类型。
- **5. 假设提取的最小 API**：add/find/remove(expected identity) 无法表达所有 session 需求；还必须加入 take(expected token)、单/多槽、detach_shutdown。
- **6. 新增抽象成本**：callbacks=至少 1（终止动作；可复用现有 session::shutdown，但 stream 不需要）；virtual=0 新增（session 的已有 virtual 保留）；config=0；policy=至少 4（key/槽位/token/停止）；new_state=0（两种 identity/token/stopping 事实均需保留）
- **7. 可删除内容/净代码**：可搬移两个 map/mutex 与约 25/105 行逻辑，但适配条件占大部；预计净删约 0–30 行，不足以抵消模板/间接层。
- **8. Correctness 风险**：旧代 cleanup 删除新代、旧控制请求 take 新 session、锁内 shutdown 引发重入、强/弱 ownership 混淆。
- **9. 测试保护**：tests/media_lifecycle.cpp:98 覆盖 20 代 identity/300 sink churn；tests/udp_transport.cpp:319 覆盖 sender token take；bench/lifecycle_verify.py:232/266 覆盖替换/DELETE。receiver/sender 各完整契约若泛化仍需新增定向测试。
- **10. 最终结论**：保持现状

### K5: 单端口/端口对 reservation 与 session release helper

- **1. 当前重复位置**：`media/net/media_port_pool.cpp:50`、`media/net/media_port_pool.cpp:67`、`media/net/media_port_pool.cpp:99`、`media/net/media_port_pool.cpp:135`、`media/rtsp/rtsp_publish_udp_session.cpp:132`、`media/gb28181/gb28181_udp_sender_session.cpp:53`、`media/gb28181/gb28181_udp_receiver_session.cpp:41`
- **2. 当前职责**：pool 唯一持有 reserved 集合，acquire/bind、address_in_use 重试、失败回滚；session 仅保留成功 reservation 的释放责任。
- **3. 是否同一 primitive**：已共用：没有两个业务模块复制 allocator/rollback；reserve 与 reserve_pair 是同组件内两种真实分配算法。
- **4. 独立业务差异**：pair 必须偶数首端口+相邻第二端口原子保留，RTSP 每 track 多 pair，GB 一 pair，WebRTC 单端口；socket close 与 release 的业务时点不同。
- **5. 假设提取的最小 API**：假设 port_lease.reset() 仅替换手写 release，不应接管 transport/session 生命周期。
- **6. 新增抽象成本**：callbacks=0；virtual=0；config=0；policy=0–1（单端口/端口对表示）；new_state=1（lease owner；替换现有 optional，不产生新事实）
- **7. 可删除内容/净代码**：约 15–25 行重复 release/optional 处理，新增 owner 类型和接入约 20–35 行，预计净变化 -5 至 +20 行；RTSP per-track cleanup 无法删除。
- **8. Correctness 风险**：多 track 部分 setup 失败、第二 socket bind rollback、close/release 顺序、异步 completion 后旧 lease 不得释放新占用。
- **9. 测试保护**：tests/udp_transport.cpp:272 与 bench/lifecycle_verify.py:330 的 GB/DTLS/端口释放路径已有保护；缺少 pool 全耗尽/第二 socket bind 失败/RTSP 第二 track rollback 的直接测试。
- **10. 最终结论**：保持现状

### K6: GB TCP active/passive 连接与 listener handoff

- **1. 当前重复位置**：`media/net/tcp_listener.cpp:15`、`service.cpp:29`、`media/gb28181/gb28181_tcp_sender_session.cpp:29`、`media/gb28181/gb28181_tcp_receiver_session.cpp:37`、`media/gb28181/gb28181_tcp_receiver_session.cpp:67`
- **2. 当前职责**：listener 的 open/bind/listen rollback 已共用；session 选择 accept/connect，建立后把 socket 移交 tcp_transport，失败清理业务注册。
- **3. 是否同一 primitive**：部分：网络 primitive 已唯一；剩下是角色相关连接阶段与业务 startup/cleanup。
- **4. 独立业务差异**：sender 建 RTP/PS 输出及长度封装，receiver 建输入解析；未连接/已连接的 registry 与资源清理不同。service 是长期多连接 accept，GB passive 是一个会话的连接建立。
- **5. 假设提取的最小 API**：假设 establish(role, endpoint, on_connected, on_error)/cancel；不能代替 session 的协议建立。
- **6. 新增抽象成本**：callbacks=至少 2（connected/error）；virtual=0；config=1（连接参数）；policy=1（active/passive）；new_state=0–1（建立阶段/取消；socket/listener 事实仍需有 owner）
- **7. 可删除内容/净代码**：可搬移约 30–50 行连接调度，但需约 30–60 行组件/适配；预计净变化 -20 至 +30 行。纯 listener cleanup helper 仅 5–10 行收益。
- **8. Correctness 风险**：accept/connect 取消、shutdown 中 socket 移交、未建连时 registry 清理；通用 base 会隐藏真实失败顺序。
- **9. 测试保护**：bench/gb_network.py:29 区分三 transport，bench/lifecycle_verify.py:208/232 覆盖 UDP/TCP active/passive 与换源；未建立连接、bind failure/cancel 专项仍需补足才能安全改连接边界。
- **10. 最终结论**：保持现状

### K7: RTCP / HLS recurring timer 泛化

- **1. 当前重复位置**：`media/rtsp/rtsp_pull_session.cpp:126`、`media/rtsp/rtsp_publish_session.cpp:313`、`media/rtsp/rtsp_publish_udp_session.cpp:217`、`media/gb28181/gb28181_udp_receiver_session.cpp:151`、`media/gb28181/gb28181_udp_sender_session.cpp:23`、`media/hls/hls.cpp:51`、`media/hls/hls_play_session.cpp:85`、`media/http/hls_http_session.cpp:158`
- **2. 当前职责**：业务条件 arm/cancel、定时 callback 的 owner lifetime 与重排；Asio 本身已提供 timer scheduling primitive。
- **3. 是否同一 primitive**：部分：相同的 Asio 调用不是相同完整 policy。
- **4. 独立业务差异**：RTCP interval/generator/endpoint/completion 链不同；HLS 100 ms 等待且 10 s 截止、30 s inactivity lease、1 s background retention cleanup 分别返回 503/删除 session/结束后台循环。
- **5. 假设提取的最小 API**：假设 arm(interval, action)/cancel；条件、重排时点与 write completion 仍需 policy。
- **6. 新增抽象成本**：callbacks=至少 2–3（action、继续条件、发送后重排）；virtual=0；config=至少 1（interval/deadline）；policy=至少 2（重排/取消结果）；new_state=0–1（终态；业务 activity/RTCP 状态必须保留）
- **7. 可删除内容/净代码**：每家族约 20–40 行 Asio 模板写法可搬移；组件/适配抵消大部，预计净删不足 20 行，不减少业务状态。
- **8. Correctness 风险**：callback-after-cancel、completion 驱动重排、RTCP 节奏、HLS HTTP 错误结果、lease 生存期被迫统一。
- **9. 测试保护**：tests/udp_transport.cpp:272 覆盖 GB sender 三 timer 与错误策略；bench/lifecycle_verify.py:266/330 覆盖 lease 到期/DTLS 异常。没有跨所有 timer 的取消竞态或精确 interval 专项。
- **10. 最终结论**：保持现状

### K8: owner shutdown / terminal fencing / worker subscription

- **1. 当前重复位置**：`media/net/worker_context.cpp:66`、`media/net/worker_context.cpp:100`、`media/gb28181/gb28181_udp_sender_session.cpp:61`、`media/rtmp/rtmp_session.cpp:265`、`media/core/worker_sink_dispatcher.h:177`、`docs/webrtc_transport_audit.md:1`
- **2. 当前职责**：worker 统一取消协程和发送 shutdown subscription；session 处理协议资源结束；sink 的 terminal fencing 防 remove 后已 snapshot callback。
- **3. 是否同一 primitive**：否：进程 worker 停止、协议终态与跨 worker 媒体 callback fencing 是不同事实。
- **4. 独立业务差异**：GB registry 会话需要响应 worker stop；RTMP/RTSP 由 connection/handler 驱动；WHEP shutdown_requested_ 是真实跨 worker fence，transport stopped 不能替代。
- **5. 假设提取的最小 API**：已有 subscribe_shutdown/reset/request_stop；再提 session base.stop() 仍需协议 cleanup callback/终态判断。
- **6. 新增抽象成本**：callbacks=至少 1（协议 cleanup）；virtual=0–1（若采用 session base）；config=0；policy=至少 1（终止时点/线程）；new_state=0–1（新增泛型 terminal；现有跨 worker fence 不能删除）
- **7. 可删除内容/净代码**：没有可删除的共同协议终态事实；预计净变化 0 至 +30 行，仅统一 post 写法不足以构成收益。
- **8. Correctness 风险**：remove/snapshot 的残余 callback、atomic fence 被删、重复 end、锁内 callback、owner 外析构、registry cleanup 顺序。
- **9. 测试保护**：tests/worker_sink_fanout.cpp:223 覆盖 snapshot/remove 语义，tests/udp_transport.cpp:272 覆盖 worker stop/重复 shutdown，bench/lifecycle_verify.py:330 覆盖真实异常结束。
- **10. 最终结论**：保持现状

### K9: codec mapping / PS-HLS stream id 小型 glue

- **1. 当前重复位置**：`media/codec/codec_utils.cpp:140`、`media/rtsp/rtsp_publish_media.cpp:195`、`media/rtsp/rtsp_pull_media.cpp:171`、`media/gb28181/gb28181_rtp_receiver.cpp:47`、`media/rtmp/rtmp_publish_session.cpp:239`、`media/ps/mpeg_ps_output.cpp:66`、`media/hls/hls_segmenter.cpp:269`
- **2. 当前职责**：外部 AV/FLV/PS enum 转换、track routing、协议支持过滤、muxer track registration；没有 queue/scheduling/资源终态 primitive。
- **3. 是否同一 primitive**：部分：纯 enum 映射可相似，但输入 enum 与 validation/support 不同。
- **4. 独立业务差异**：GB 不接受 Opus，RTSP 有特定 Opus fallback；RTMP 分 config/raw；PS 与 HLS 对 G711 校验和 muxer 重建位置不同。
- **5. 假设提取的最小 API**：假设 codec_from_avpacket(int)/psi_stream_id(codec_id)；调用方必须继续保留支持过滤、track validation 和 muxer add。
- **6. 新增抽象成本**：callbacks=0；virtual=0；config=0；policy=0–1（允许 codec 的调用方规则必须保留）；new_state=0
- **7. 可删除内容/净代码**：每组小 switch 预计净删 5–15 行，0 个 ownership/scheduling 字段消失；通用时间转换已集中 codec_utils，无再次抽取收益。
- **8. Correctness 风险**：把 codec mapping 与支持承诺混为一体，误扩大 Opus/G711 支持；改变整数时间截断或 wrap 语义。
- **9. 测试保护**：tests/rtmp_decode.py:43/48 检查 H264/H265 与 AAC 解码；tests/media_lifecycle.cpp:177 检查派生 track；未覆盖各 enum/unknown/G711/时间边界。
- **10. 最终结论**：保持现状

### K10: avpkt2bs C handle / payload copy glue

- **1. 当前重复位置**：`media/rtsp/rtsp_pull_media.cpp:42`、`media/rtsp/rtsp_pull_media.cpp:162`、`media/rtsp/rtsp_publish_media.cpp:45`、`media/rtsp/rtsp_publish_media.cpp:222`、`media/gb28181/gb28181_rtp_receiver.cpp:78`、`media/gb28181/gb28181_rtp_receiver.cpp:277`、`media/webrtc/whip_media_receiver.h:59`、`media/webrtc/whip_media_receiver.cpp:226`、`third/ireader/avbsf/include/avpkt2bs.h:48`、`third/ireader/avbsf/src/avpkt2bs.c:9`
- **2. 当前职责**：四个生产调用方持有 codec config 缓存和重用 scratch buffer，input 后复制到共享不可变媒体 payload，终止时 destroy。framing/realloc 算法已经唯一在依赖 avpkt2bs 实现。
- **3. 是否同一 primitive**：是（小型 handle glue）；不是四份 codec framing 或 async ownership 算法。
- **4. 独立业务差异**：RTSP pull/GB config 更新时 reset，publish/WHIP 已就绪后 config change 结束；返回负/零/空 ptr 的传播及转码位置不同。
- **5. 假设提取的最小 API**：若确需封装，仅 reset()/input(packet)->status+bytes/view；已有依赖 AVPacket2BitStream 提供 Input/析构，但没有完整 reset/shared payload 输出 API。不能再实现 framing。
- **6. 新增抽象成本**：callbacks=0；virtual=0；config=0；policy=0；new_state=0（只是把现有 avpkt2bs_t 搬进 wrapper）
- **7. 可删除内容/净代码**：create/destroy/reset 约可替换 20–30 行，新窄 wrapper 约 25–35 行，预计净增 5–15 行；若复用依赖 wrapper，reset/错误/shared payload 适配仍保留，预期净删不超过 20 行。
- **8. Correctness 风险**：view 在下一次 input/reset 后失效；负值/零值语义、config reset、buffer copy 到共享 payload 必须保持。WHIP bitstream_{} 等价于依赖 create 的 memset，不构成初始化缺陷。
- **9. 测试保护**：真实 RTSP/WHIP/GB smoke/replacement 间接覆盖；没有 malformed packet、config reset、零 payload、scratch view lifetime 的直接 tests。没有真实 bug 证据推动此封装。
- **10. 最终结论**：保持现状

### K11: media_stream / MPEG-PS 剩余 add/remove 入口写法

- **1. 当前重复位置**：`media/core/media_stream.cpp:39`、`media/core/media_stream.cpp:50`、`media/core/media_stream.cpp:60`、`media/ps/mpeg_ps_output.cpp:137`、`media/ps/mpeg_ps_output.cpp:148`、`media/ps/mpeg_ps_output.cpp:158`
- **2. 当前职责**：业务 owner 获取 sink target worker、dispatch 到 source owner、判断 ended/muxer/source；仅委托统一 dispatcher。
- **3. 是否同一 primitive**：部分：dispatch 写法相同，但完整 group queue/snapshot/drain/end 算法已经只有一份；剩余是业务入口适配。
- **4. 独立业务差异**：media_stream 用 ended_，PS 用 source_/muxer_；late add 向 sink worker post END，target 在跨线程前取出，source owner 的强 lifetime 由入口捕获。
- **5. 假设提取的最小 API**：现有 dispatcher.add/remove/publish/end；若强抽入口，需 owner capture 和 terminal predicate/late-end policy，不能再只收 Frame/Sink。
- **6. 新增抽象成本**：callbacks=至少 1（业务 terminal predicate，除非增加 traits/policy）；virtual=0；config=0；policy=至少 1（业务 ended/late add）；new_state=0–1（错误做法是重复 ended；正确抽法仍需原业务状态）
- **7. 可删除内容/净代码**：仅约 20–35 行相似入口可搬移，新增适配约 30–50 行，预计净变化 -5 至 +30 行。业务字段不能删。
- **8. Correctness 风险**：把业务终态放进 dispatcher、引入新 runtime callback、target worker 获取/late END 时序改变、post 数增加。
- **9. 测试保护**：tests/worker_sink_fanout.cpp:152/184/223/305 同时实例化 canonical 与 PS，已有 inline/order/weak/remove/overflow 边界；现有实现无需进一步适配。
- **10. 最终结论**：保持现状

### K12: WHEP/WHIP common WebRTC transport（沿用冻结结论）

- **1. 当前重复位置**：`docs/webrtc_transport_audit.md:1`、`media/webrtc/whep_session.h:1`、`media/webrtc/whip_session.h:1`
- **2. 当前职责**：先前已完整审计 UDP/STUN/ICE-Lite/DTLS/timers/endpoint/port/credentials；本轮只读取既有报告，未重复协议状态机审计。
- **3. 是否同一 primitive**：部分：存在真实共同网络实现，但媒体/ready 确认/终止 ownership 差异尚需跨边界协调。
- **4. 独立业务差异**：WHEP 跨 worker terminal fence、WHIP source receiver、不同 SDP/媒体建立时点必须保留；WHEP/WHIP weak registry 也不改泛型 registry。
- **5. 假设提取的最小 API**：沿用报告的 bind/start/write/request_stop/shutdown；完整设计见原报告，本轮不再扩展。
- **6. 新增抽象成本**：callbacks=3（ready 确认、media、failure；旧报告估算）；virtual=0；config=初始化凭证/绑定信息（旧报告）；policy=业务 ready/生命周期确认不能删除；new_state=传输终态迁移，WHEP atomic fence 等仍保留（旧报告）
- **7. 可删除内容/净代码**：沿用既有 12 对方法约 549 行、10 个网络字段的识别；估计净删 80–180 行，尚未 prototype，不视为本轮新测量或新提取机会。
- **8. Correctness 风险**：ready callback 确认、establishment timer 取消时点、媒体 callback-after-remove、凭证/端口归属；新证据不足以推翻旧否决。
- **9. 测试保护**：docs/webrtc_transport_audit.md 的历史生命周期/WHEP 1000/ASan 输入仍有效作为冻结输入；本轮未执行新测试。
- **10. 最终结论**：保持现状

## C. 需要真实需求触发后再考虑


### F1: source-generation 派生输出共享 cache

- **1. 当前重复位置**：`media/core/media_stream.cpp:93`、`media/hls/hls.cpp:72`、`media/webrtc/whep_audio_egress.cpp:143`、`media/webrtc/whep_audio_egress.cpp:169`
- **2. 当前职责**：按 source identity 懒创建派生输出，成功后缓存、失败 rollback，结束/最后引用时释放。
- **3. 是否同一 primitive**：部分：共同概念是 generation 隔离，但三种 ownership/retention/release 算法不同。
- **4. 独立业务差异**：PS source 内 weak 单实例；HLS name map+weak source+strong segmenter，结束后保留且 source 不在时能返回旧实例；WHEP source+settings weak map、viewer lease、最后 release dispatch finish。
- **5. 假设提取的最小 API**：只可假设 get_or_create(source_identity, key, factory)/release(lease)；要覆盖 HLS retention 和 PS 无 lease，仍需独立 policy，当前不定义完整框架。
- **6. 新增抽象成本**：callbacks=至少 2（factory/startup、finish；保留策略另计）；virtual=0；config=0 新通用配置；调用方 settings/retention 原事实保留；policy=至少 3（cache 强弱、retention、lease）；new_state=0–2（泛型 lease/entry；原 generation/settings 不能删）
- **7. 可删除内容/净代码**：三边可搬移约 50–85 行 cache/acquire/release，但新增组件和适配约 45–90 行，预计净变化 -40 至 +40 行；不是直接移除三份同算法。
- **8. Correctness 风险**：同名跨代复用、raw source pointer 地址复用、last release 与新 acquire、锁内 startup、source/derived strong cycle、HLS ended retention。
- **9. 测试保护**：tests/media_lifecycle.cpp:177 对同实例/跨代/最终 source 释放已有直接保护；lease release 与新 acquire 竞态、不同 settings、factory 失败等仍缺精准覆盖。
- **10. 最终结论**：需要真实需求后再做

**触发条件**：出现另一个具有相同 cache+lease+retention policy 的长期生产输出，或同类 generation/lease bug 在两个输出中重复发生，再重新比较最小接口与净状态收益。

### F2: ireader rtsp_demuxer raw handle RAII

- **1. 当前重复位置**：`media/rtsp/rtsp_pull_media.cpp:53`、`media/rtsp/rtsp_pull_media.cpp:133`、`media/rtsp/rtsp_publish_media.cpp:50`、`media/rtsp/rtsp_publish_media.cpp:165`、`media/gb28181/gb28181_rtp_receiver.cpp:79`、`media/webrtc/whip_media_receiver.cpp:46`、`media/webrtc/whip_media_receiver.cpp:178`
- **2. 当前职责**：C handle create/destroy、startup 部分成功 rollback；demux/input 算法本来就在依赖中唯一实现。
- **3. 是否同一 primitive**：部分：销毁 primitive 相同；配置、vector/双 handle、callback this 与失败清理不同。
- **4. 独立业务差异**：RTSP 多 track vector、GB PS notify、WHIP 音视频双 demuxer+SR 时间同步；startup 顺序和 callback 生命周期不能封装成通用业务 startup。
- **5. 假设提取的最小 API**：仅 unique_ptr<rtsp_demuxer_t, deleter> / owner.reset()，不增加 parser/startup base。
- **6. 新增抽象成本**：callbacks=0 新增（现有 demuxer callback this 仍保留）；virtual=0；config=0；policy=0（仅 deleter；业务规则留调用方）；new_state=0（raw handle 换 owner，未增加事实）
- **7. 可删除内容/净代码**：约 15–25 行 destroy/置空，新增 alias/deleter/初始化约 10–20 行，预计净变化 -15 至 +5 行；只有小型 resource glue，未发现完整调度算法复制。
- **8. Correctness 风险**：startup 部分失败、callback this 比 handle 先析构、vector/double demuxer reset 时点；RAII 不自动解决回调所在对象 lifetime。
- **9. 测试保护**：真实 RTSP/WHIP/GB smoke/replacement/disconnect 间接保护；没有部分初始化失败/反复 reset/回调释放的 direct unit coverage。
- **10. 最终结论**：需要真实需求后再做

**触发条件**：出现可复现的漏 destroy/部分初始化 rollback bug，或新增模块确实重复同一 handle-owner 契约；修复时优先已有依赖/标准 RAII，不建业务通用 pipeline。

## Correctness findings 与排除的线索

**确认的问题：无。** 这是本轮证据范围内的结论，不是“所有路径绝无缺陷”的证明。本次没有修复生产问题。此前 30c09f4 已修复 H265 RTMP/HTTP-FLV 互操作，属于基线历史，不记作本次新发现；“无”不否认历史上存在并已修复的问题。

- **WHIP 未调用 avpkt2bs_create**：bitstream_{} 对 POD 成员/union 初始状态清零；create 仅 memset，destroy 仅 free 非空 ptr。没有未初始化资源/不成对创建缺陷，不能用此线索推动 wrapper。 证据：`media/webrtc/whip_media_receiver.h:59`、`third/ireader/avbsf/src/avpkt2bs.c:9`、`third/ireader/avbsf/src/avpkt2bs.c:15`。
- **HLS shutdown source_ 重复清空 / HTTP-FLV shutdown 未清 queue**：HLS replacement shutdown 后立即移除 entry，global shutdown 后 clear；on_end/finish 不清 source，未找到当前生产路径二次 shutdown 的证据。FLV 当前 queue front 供 yield async_write 使用，保留到 completion/error 清理满足 buffer lifetime；提前清 queue 才危险。均不记为 confirmed bug。 证据：`media/hls/hls.cpp:92`、`media/hls/hls.cpp:105`、`media/hls/hls_segmenter.cpp:83`、`media/hls/hls_segmenter.cpp:124`、`media/http/http_flv_session.cpp:190`、`media/http/http_flv_session.cpp:274`。

## 测试覆盖分析（未执行）

### CTest 注册

在本报告审计基线 `30c09f4`，BUILD_TESTING 下 FFmpeg 可发现时 10 个：worker_sink_fanout、udp_transport、media_lifecycle、rtmp_decode、rtmp_hevc_decode、help、4 个 invalid 参数；无 FFmpeg 为 8 个。首次审计基线可发现 FFmpeg 时为 9 个。这些是历史注册数量，不代表当前 checkout，历史验证数量不改写。

缺口/限制：本轮只读注册/断言，未执行；不能宣称本轮 10/10。

证据：`CMakeLists.txt:313`、`CMakeLists.txt:326`、`CMakeLists.txt:333`。

### UDP

40 datagram 顺序/endpoint/span-vector ownership、100 次在途 shutdown、1 MiB admission/reentry、真实 send error/rebind 旧 completion、GB sender overflow/error/timers/registry/lifetime。

缺口/限制：RTSP/WHIP/WHEP 具体错误 policy 主要依靠真实协议 suite；不把 GB policy direct assertions 外推为每个协议所有错误均已单测。

证据：`tests/udp_transport.cpp:68`、`tests/udp_transport.cpp:127`、`tests/udp_transport.cpp:176`、`tests/udp_transport.cpp:230`、`tests/udp_transport.cpp:272`。

### worker fanout

canonical/PS 两种模板，同 worker inline、多 target/多 sink、单 drain/order/shared payload、weak/remove/snapshot、2500 boundary/overflow/end/其他 group 独立。

缺口/限制：已有保护足以支持现有窄边界；并非所有未来泛型 lifecycle policy 的证明。

证据：`tests/worker_sink_fanout.cpp:152`、`tests/worker_sink_fanout.cpp:184`、`tests/worker_sink_fanout.cpp:223`、`tests/worker_sink_fanout.cpp:305`、`tests/worker_sink_fanout.cpp:370`。

### generation/派生资源

20 代同名 source 对象 identity、300 sink churn、PS/HLS/AAC→Opus 同代共享及跨代不共享、旧 HLS ended、最终 source 释放；GB sender session token take。

缺口/限制：WHEP egress 不同 settings、last release/acquire overlap 与 factory 失败没有同等 direct assertions。

证据：`tests/media_lifecycle.cpp:98`、`tests/media_lifecycle.cpp:177`、`tests/udp_transport.cpp:319`。

### 真实协议保护

现有 smoke/replacement/churn/disconnect 可验证 RTMP/RTSP/WHIP 输入、多协议输出、GB UDP/TCP active/passive、旧 viewer/end/lease/delete/port release。既有验证文档记录 normal/ASan/UBSan 与容量的历史运行。

缺口/限制：本轮未运行这些 suite；历史失败/复测说明保留在原文档，不重新包装为全零失败或性能优化。

证据：`bench/lifecycle_verify.py:208`、`bench/lifecycle_verify.py:232`、`bench/lifecycle_verify.py:266`、`bench/lifecycle_verify.py:330`、`bench/gb_network.py:29`、`docs/worker_sink_dispatcher_verification.md:96`。

### TCP / HTTP / HLS

FFmpeg RTMP H264/H265 与 AAC 解码断言、FLV header/tag/read、HLS replacement/lease expiry、TCP 异常断开与回收。

缺口/限制：没有 TCP transport 串行 queue/overflow/in-flight 专项；FLV chunk 精确编码/慢 reader、HLS segment window/ENOBUFS/在途 body 缺定向断言。

证据：`tests/rtmp_decode.py:43`、`bench/http_flv_fanout.py:30`、`bench/lifecycle_verify.py:266`、`bench/lifecycle_verify.py:330`。

### codec glue / transcoder / timers

Opus track metadata、H264/H265 与 AAC FFmpeg 解码断言、GB sender timer/error lifecycle。

缺口/限制：没有 avpkt2bs malformed/reset/view、AAC→Opus 实际编码内容/采样率/声道/flush、codec enum/时间 wrap 边界、各 timer cancel 竞态专项。测试缺口不等价于生产 bug 或抽象需求。

证据：`tests/media_lifecycle.cpp:199`、`tests/rtmp_decode.py:18`、`tests/udp_transport.cpp:272`。

### 新增 codec 互操作工具的证据边界（未执行）

`bench/codec_interop_verify.py:57` 复用现有 Run 和 GB pair；`:64` 开始逐输出调用独立 FFmpeg，`:82` 检查解码后的 PTS，`:87` 同时要求退出码、实际帧数、codec 和时间戳断言。它是未注册到 CTest 的独立验证工具，没有新增生产 scheduler 或资源 allocator。

GB 分支经过服务器 sender→真实网络→receiver relay，再由 FFmpeg 经 RTSP 播放解码；这不是独立 GB 设备/SIP 平台互操作证明。脚本能力不等于所有 codec/输出组合已执行通过。本次不读取主工作区未提交版本来替代已提交基线，也不将既有产品测试运行归入本审计。

历史结果曾记录于 `docs/stability_verification.md`、`docs/udp_transport_verification.md`、`docs/worker_sink_dispatcher_verification.md` 与 `docs/performance_baseline.md`，这些历史报告已按清理要求移除，可在 Git 历史中查阅。fanout 文档当时记录普通/ASan/UBSan 18 套真实协议回归及 WHEP 100/500/1000；也保留首轮 GB 测试源异步删除问题和容量竞争失败/复测。它们是既有验证，**本次没有再次声称 CTest、sanitizer PASS 或零丢包。**

测试缺口本身不证明抽象失败，不自动产生生产改造任务。未来实际修改 TCP、codec glue 或 lease 边界时，再围绕对应公共行为补测试。

## 最终建议与交付边界

当前架构级主动精简基本完成。停止主动寻找 DRY 重构；生产修改由真实需求、可复现 bug、协议互操作或性能数据驱动。未来触发 F1/F2 时重新审接口收益，并在实际改动前补对应公共边界测试。

当前已有同职责的完整算法均由基础设施持有；剩余相似写法没有同时满足净状态下降、唯一事实来源、低 callback/policy 成本和行为不变的门槛。无需为了 DRY 增加 generic queue、session base、registry 模板、timer framework、媒体 pipeline、GOP/history/reader。

本次仅更新并提交以下两份文档，提交信息为“记录基础设施重复设计审计”：

- `docs/infrastructure_duplication_audit.md`
- `docs/verification_results/infrastructure_duplication_audit.json`

提交前校验 JSON、证据路径/行号、protected tracked 文件 SHA256 与 git diff/cached diff；提交后 fetch/status/HEAD 对 origin/main。隔离工作区按 clean/远端同步验收；原工作区三项既有测试工具改动单独核对内容未变，保留其 dirty 状态。最终提交 SHA、clean/远端同步结论在交付回报中给出，避免把文档自身 SHA 写回而产生循环提交。

首次审计临时证据：`/tmp/media_server_infrastructure-acee8da`。本次证据：`/tmp/media_server_infrastructure_audit-30c09f4-evidence`（baseline/history、七类搜索及与原基线比较、protected-files manifest、原工作区改动 hash 与最终 git 检查）。当时 JSON 是本报告的结构化结果，不代表新增运行时验证；上述临时证据与 `docs/verification_results/infrastructure_duplication_audit.json` 已按清理要求移除，路径仅用于定位历史记录。
