# 媒体身份与生命周期

## 三类身份

| 身份 | 含义与寿命 |
| --- | --- |
| 业务源 | `source_id`、`push_device_id`、GB device/channel 标识可持续存在的配置或设备；显示名不是运行实例身份 |
| 媒体源 | `stream_id` 标识一次推流、拉流或历史媒体生命周期，全生命周期唯一，结束后不允许复用；重新开始必须获得新 ID |
| session / 本地 generation | 服务端播放 session 标识独立消费者；播放器 generation 或请求 context 标识本地一次操作，用于隔离迟到结果 |

Go 为主动源和 GB 会话分配新 UUID；push 的一次性发布凭据本身作为 `stream_id`，数据库原子记录消费。GB live 当前用同一 UUID 表示 `live_id` 与 `stream_id`；历史实例当前用同一 UUID 表示 `playback_id` 与 `stream_id`。数值相同不改变业务、媒体和播放资源的职责区别。

WHEP/WHIP 返回的 `Location`、HLS 的播放 secret、GB sender 的 `sender_id` 是各自资源身份。浏览器 [`whep.js`](../signaling/web/whep.js) 的 `generation` 只是本地递增操作号；Qt 使用可销毁的请求 context 与独占历史请求对象隔离旧回复。它们均不能代替服务端 `stream_id`，也不能授权复用旧播放票据。

## 实时源

1. 协议入口完成必要授权并创建 receiver。此时 receiver registry 可见，媒体可能尚未就绪。
2. 输入收集完整固定 tracks，`media_stream::set_tracks()` 只接受一次非空、非重复轨道集合，随后注册 source。
3. 输入发布 canonical frame；sink 根据固定轨道初始化并等待自然关键帧。没有通用历史回放或反向关键帧请求。
4. 输入结束、超时或配置变化时，owner 结束源、移除自身注册并关闭相关资源。再次接入属于新生命周期，使用新 `stream_id`。

[`stream_registry.cpp`](../media/core/stream_registry.cpp) 只保存活跃注册项，不维护永久的“已用 ID”台账；全生命周期唯一由上层身份分配和一次性凭据契约保证。对象 identity 检查用于保护重复请求和迟到清理，不能理解为允许同名重启。

AAC→Opus 派生流沿用源的 `stream_id` 作为内部关联，不另注册成新的外部源；它和上游是不同对象。共享键包含源对象身份及音频参数，不能只凭字符串跨生命周期共享。

## 分发与关闭

- `worker_sink_dispatcher` 在 source worker 串行处理 add/remove/publish/end。同 worker 直接调用，跨 worker 按目标分组排队；目标 worker 的 drain 逐帧交付已排队帧，单次执行超过 20 ms 后重新投递，让同一事件循环的控制请求和会话建立交替执行；时间片内积压的多帧仍连续交付，sink 可以把它们合并写出。正常 end 在已排队帧之后交付。待交付帧达到 2500 帧上限时丢弃该组积压并结束慢组。
- remove sink 不撤销已经取得的 snapshot 或已执行的投递。sink 必须用自己的资源终态处理迟到 frame/end。
- session 的 `shutdown()` 通常向 owner 投递，`safe_shutdown()` 执行清理；父对象调用子对象 `shutdown()`，不把它当作同步释放屏障。
- 关闭时取消 timer/协程、解除订阅、断开回调持有并释放资源。异步 I/O 的成功完成可能与取消交错；RTSP/RTMP pull 在恢复边界检查取消、资源及 receiver identity。
- transport 持有串行写队列和在途 buffer，清空队列不提前释放 completion 所需数据。TCP 写在 owner worker 上直接入队并检查 1 MiB 上限；上一次写完成后，队列中的积压一次写出，发送能力不受事件循环轮次限制。UDP 队列为空时直接非阻塞发送，只有 EAGAIN 才入队；回调只报告错误。入队拒绝与 socket 发送错误是不同结果，上层决定会话策略。

服务停机由 [`service.cpp`](../service.cpp) 处理 SIGINT/SIGTERM：关闭 WHIP/WHEP registry、停止已登记 receiver/sender，再请求 worker 停止。`worker_context` 发出任务取消、调用 shutdown subscription 并释放 work guard，让清理完成后退出事件循环。

## 播放与共享资源

RTMP 先 prepare codec 初始化包，成功发送 Play.Start 后才启动输出；RTSP 先成功回复 PLAY，再订阅媒体。拥有 source 不等于已经开始播放。

关闭普通 viewer 只移除该消费者，不停止共享输入或 GB live。停止 source/live 则会使相关实时消费者收到源结束。PS 共享 muxer，GB sender 保持独立 RTP 和传输状态；WHEP 共享 AAC→Opus 计算，保留独立加密和网络状态。

HLS 先消费一次性授权，再获取共享 segmenter 和创建独立 play session；入口重定向到该 session 的 playlist。源结束后仍按 retention 保留片段，播放 session 按活动超时清理。这是 HLS 的协议保留，不是媒体核心的 history。

## 历史实例、下载与音频上传

历史播放/下载先创建独占 TCP passive receiver，返回一次性 PS URL；消费者认领授权后才触发设备 INVITE。取消、认领超时、绝对 deadline 或传输结束会结束本次实例。pause/resume/seek/scale 在同一 SIP dialog 内发送 INFO，暂停仍占用绝对期限。HTTP 完整结束仅表示该传输完成，不证明设备录像库存完整。

Qt 的 [`history_request.cpp`](../desktop/history_request.cpp) 让每次历史请求拥有独立 HTTP client，取消后迟到的创建回复只删除本次 `playback_id`。实时观看、独立录制和历史下载各自拥有票据；停止画面不应误删其他实例。录制正常收尾，下载失败/取消清理本次临时文件。

对讲是设备级独占 `talk_id`。客户端先准备采集，再申请一次性 WebSocket 上传 URL；取消和迟到响应只回收匹配的 owner。上传建立、音频已发送、设备实际听音是不同事实。

源码入口：`media/core/`、`media/net/worker_context.*`、`media/gb28181/gb28181_playback_session.*`、`signaling/live_session.go`、`signaling/rtsp_pull_session.go`、`signaling/gb_playback.go`、`signaling/gb_talk.go`。
