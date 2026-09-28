# 媒体核心边界

`media_stream` 表示一个 source generation。输入协议先收集完整的固定轨道，调用 `set_tracks()` 后再将 stream 放入 registry；stream 对外可见后，轨道、codec 和 codec config 在整个 generation 内不再变化。配置变化意味着旧 generation 结束并创建新的 stream。

媒体核心只做一件事：把 canonical `media_frame` 推送给 sink。

```text
输入协议 → canonical media_stream
                         ├─ RTMP sink
                         ├─ RTSP sink
                         ├─ HTTP-FLV sink
                         ├─ WHEP sink
                         ├─ HLS sink
                         ├─ 共享 AAC→Opus sink → media_stream
                         └─ MPEG-PS sink → GB28181 sender
```

`media_stream` 保存固定 tracks、source worker 和按目标 worker 分组的 sink。source worker 上的 sink 直接调用；其他 worker 每个 frame 最多投递一个 drain handler，handler 在目标 worker 内依次调用该组 sinks。跨 worker 的临时 pending frame 队列有固定上限，溢出时结束该组，避免把慢 sink 变成无界 Asio handler backlog。`media_frame` 的 payload 使用共享 buffer，fanout 不为每个 sink 复制媒体内容。

sink 是具体的 push 接口：

```cpp
class media_sink {
public:
    virtual ~media_sink() = default;
    virtual worker_context& worker() noexcept = 0;
    virtual void on_frame(const media_frame&) = 0;
    virtual void on_end() = 0;
};
```

sink 不读取历史、不拥有 cursor，也不等待媒体。tracks 由 consumer 启动时直接从 source 读取。网络输出自己的 write queue、背压和关闭策略仍由协议 session 负责；media core 不为慢 viewer 保存 replay history，也不反压 publisher。

source end 在每个 worker 组内排在此前已经排队的 frame 之后，再调用 sink 的 `on_end()`。sink 可以在回调中关闭自身；stream 在 owner worker 串行处理注册和移除，跨 worker 的 queued callback 只依赖 sink 自身的 closed 状态。

HLS 是 source worker 上的普通 sink，继续保留 MPEG-TS 分段、playlist 和结束后的片段 retention。AAC→Opus 是按 source generation 与音频参数共享的派生 sink，输出新的 `media_stream`。MPEG-PS 是 GB28181 模块内的共享派生输出：一个 source generation 只创建一个 PS muxer，再把 PS frame 推送给多个 sender；每个 sender 独立维护 RTP、SSRC、sequence 和传输状态。

核心没有 reader、pull API、GOP replay、cursor、runtime track event、AV1 或通用 `Frame` template。协议层只接收真实媒体帧和 source end，任何 codec/config 变化都通过结束当前 generation 表达。
