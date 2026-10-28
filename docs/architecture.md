# 架构

media_server 由 C++ 媒体服务、Go 信令服务和可独立构建的 Qt 客户端组成。信令决定接入、授权和业务会话；媒体服务处理协议与媒体；客户端拥有自己的播放请求和呈现状态。

## 模块职责

| 模块 | 职责与入口 |
| --- | --- |
| 服务与执行 | `main.cpp`、`config.cpp`、`service.cpp` 创建监听器和 worker；`media/net/` 管理 I/O、取消、端口和传输队列 |
| 媒体核心 | `media/core/` 管理固定轨道、源注册、session 注册及 sink 分发 |
| 协议边界 | `media/rtmp/`、`media/rtsp/`、`media/webrtc/`、`media/gb28181/` 负责协商、解析、封装与各自生命周期；`media/http/` 分派控制和媒体请求 |
| 派生输出 | `media/flv/` 共享 FLV tag 封装；`media/hls/` 生成 TS 分片；`media/ps/` 共享 PS 封装；`whep_audio_egress` 共享 AAC→Opus 转码 |
| 信令 | `signaling/` 管理 SQLite 配置、SIP 设备、源运行实例、授权和媒体状态对账；静态控制台由 `web.go` 嵌入二进制 |
| 桌面端 | `desktop/` 使用信令 API，实时播放走 RTSP/TCP，历史回放走有限 HTTP PS；FFmpeg 解码、Qt 呈现、SDL 音频 |
| 工具 | `bench/` 提供负载、容量搜索、采样及运行包工具；`signaling/simulator/` 是独立 GB 设备/媒体模拟器 |

## 实时媒体流转

```text
发布 / 主动拉流 / GB PS-RTP
  → 协议解析与轨道收集
  → 固定 tracks 的 media_stream
      ├─ RTSP / WHEP sink
      ├─ 共享 FLV tag → 各 RTMP / HTTP-FLV viewer
      ├─ HLS segmenter → playlist / TS
      ├─ 共享 AAC→Opus → 内部派生 media_stream → WHEP
      └─ 共享 MPEG-PS → 各 GB sender 独立 RTP / 网络传输
```

内部数据定义见 [`media_types.h`](../media/core/media_types.h)：H.264/H.265 为 Annex-B access unit，AAC 为完整 ADTS frame，Opus 为单 packet；DTS/PTS 使用纳秒，payload 是共享只读 buffer。视频帧另带按需转换一次的长度前缀副本，供 RTSP/WHEP 的 RTP 打包共享，避免每个 viewer 重复扫描起始码。轨道保存 codec、时钟、声道及固定 codec config。协议支持范围见 [协议](protocols.md)。

输入必须先收集完整轨道，再调用 `set_tracks()` 和注册源。一个 `media_stream` 对象只代表一代源，轨道不能在运行中更新。媒体核心按源推送，不保存 GOP/history，也不反向要求关键帧；新 viewer 等待自然关键帧。

`stream_registry` 保存已经有轨道的源，`session_registry` 保存可停止的 receiver/sender。receiver 已存在不代表媒体已就绪。两者删除均检查对象身份，避免失败或迟到对象清理错误的注册项。`stream_id` 全生命周期唯一，结束后不得复用；详细身份与关闭规则见 [媒体生命周期](media-lifecycle.md)。

## 执行与共享边界

每个 worker 拥有一个 `io_context`，由一个线程运行。连接按轮转分配，session 的资源在所属 worker 操作。WHIP 发布会话和 WHEP 拉流会话要同步完成 Opus→AAC 转码，单路成本是普通连接的数十倍，因此不跟随 HTTP 连接，而是分别放到现有同类会话最少的 worker；会话对象在启动接收协程前构造完成，此后只在该 worker 上操作。WHEP 播放会话同样放到播放会话最少的 worker，按源共享的 AAC→Opus 转码器另按转码器数量放到最少的 worker，其关闭订阅在所属 worker 上登记。同 worker 的 sink 直接收到帧；跨 worker 按目标 worker 分组，共享有界队列和 drain 调度，算法位于 [`worker_sink_dispatcher.h`](../media/core/worker_sink_dispatcher.h)。

TCP/UDP transport 负责 socket、串行写入和在途 buffer，上层 session 决定错误后的关闭策略。HTTP-FLV 的 HTTP 输出队列、HLS 的片段保留分别具有协议职责。

共享派生计算不共享连接身份：AAC→Opus 以源对象及音频参数为键；PS、FLV tag 输出由源对象持有弱引用并按需创建。FLV tag 携带当时的 sequence header，viewer 从起播点先补发它们。每个 viewer/sender 的 RTMP chunk、RTP sequence、SSRC、DTLS/SRTP、端点和网络状态独立。

## 有限媒体与控制面

GB 历史回放/下载由 `gb28181_playback_session` 接收 TCP/RTP-PS，通过有限 HTTP chunked PS 输入交给一个消费者，使用背压和结束标记。它不经过实时源的普通 fanout 模型。设备音频上传由 `gb28181_audio_session` 处理 WebSocket PCMA 和独立的 GB 音频发送会话。

Go 保存设备/源配置及推流凭据消费事实；live、播放票据、通道状态和报警等运行状态主要在内存。`media_reconcile.go` 定期查询媒体 receiver 并对账，`media_ready` 是最近对账的轨道就绪结果，不表示客户端已解码或设备已执行控制。API 与使用顺序见 [信令 API](signaling-api.md)。

构建与部署见 [build-deploy.md](build-deploy.md)，测量入口见 [performance.md](performance.md)。当前文档描述实现边界，不给出跨设备兼容或容量验收结论。
