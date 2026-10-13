# media_server 工程上下文

首次接手项目读本文；提出设计改动时读 [decisions.md](decisions.md) 和 [constraints.md](constraints.md)；排查重复问题时读 [lessons.md](lessons.md)；交接进度与测试范围读 [current.md](current.md)。五份文档完整说明核心取舍，并引用仓库源码、提交和已跟踪报告，不替代当前明确需求与源码核对。

在 `813b4f1` 文件树中尚无项目 `AGENTS.md` 入口，旧个人规则已完整备份到工作树外；本次根目录 `AGENTS.md` 仅路由到版本控制中的项目文档。未跟踪的本地 `docs/project_decisions.md` 只补充历史讨论，不是读取本知识库的前提，也不作为唯一权威来源。

`docs/development_conventions.md` 中指向旧 `docs/project_decisions.md` 的架构阅读要求，现由本文、[decisions.md](decisions.md) 和 [constraints.md](constraints.md) 承担；克隆仓库后不需要取得该本地文件。

## 项目目标

项目是最小、可维护的多协议媒体服务器：将输入转为统一媒体帧，再供不同协议输出。目标是保持已验证的端到端能力和清晰的资源所有权，不追求功能堆积、通用媒体图或完整浏览器媒体控制器。

既定产品边界为视频源、可选音频。内部存在 AAC、Opus、G.711 类型和 HLS 音频分支，不等于已承诺纯音频 publisher 或所有 codec 的任意协议组合。

依据：已跟踪的 [媒体核心边界](../docs/media_core_architecture.md)、[协议客户端边界](../docs/performance_tools/protocol_clients.md)和当前 `media/core/media_types.h`；本地 `docs/project_decisions.md` §1 仅补充历史讨论。

## 架构与职责

| 层 | 职责 | 主要位置 |
| --- | --- | --- |
| Go signaling | 设备、通道、GB SIP/live、RTSP 源管理、一次性发布/播放授权及媒体状态对账 | `signaling/` |
| 协议入口与 session | 校验和协商、协议解析/封装、端点选择、运行期生命周期及错误策略 | `media/rtmp/`、`media/rtsp/`、`media/webrtc/`、`media/gb28181/`、`media/http/` |
| 媒体核心 | 固定轨道的 source generation、帧推送、源注册及结束 | `media/core/` |
| 共享派生输出 | HLS 分片、AAC→Opus egress、MPEG-PS mux | `media/hls/`、`media/webrtc/whep_audio_egress.*`、`media/ps/` |
| 网络与执行层 | owner worker、取消、TCP/UDP I/O、写队列及 buffer lifetime | `media/net/`、`service.cpp` |

`stream_registry` 保存媒体源，`session_registry` 保存可控制的接收/发送会话，两者不代表同一事实。删除时按对象 identity 判断，避免旧会话移除同名的新源。

## 当前协议

| 协议 | 输入 | 输出 |
| --- | --- | --- |
| RTMP | TCP Publish | TCP Play |
| RTSP | TCP interleaved / UDP 发布；TCP interleaved Pull | TCP interleaved Play |
| GB28181 | UDP、TCP active/passive PS-RTP receiver；Go 侧 SIP 设备点播 | UDP、TCP active/passive PS-RTP sender |
| WebRTC | WHIP，ICE-Lite / DTLS / SRTP | WHEP，ICE-Lite / DTLS / SRTP |
| HTTP | 控制与授权相关入口 | HTTP-FLV、HLS |

RTSP UDP 发布不能推导为 UDP Play/Pull。codec 和参数是否可用由实际协议协商决定，第三方库支持某 codec 不等于项目对外支持。

依据：当前协议 session、`rtsp_pull_session::rtp_port_callback()`、`rtsp_play_session::on_setup()`；[协议客户端说明](../docs/performance_tools/protocol_clients.md)记录工具能力及媒体检查边界，不代表所有输入×输出组合都已实测。

## 核心数据流

```text
输入协议解析 / 解复用
  → 完整固定 tracks → 注册 media_stream（一个对象 = 一代源）
  → canonical media_frame
      ├─ RTMP / RTSP / HTTP-FLV / WHEP sink
      ├─ HLS segmenter
      ├─ 共享 AAC→Opus egress → 派生 media_stream → WHEP
      └─ 共享 MPEG-PS output → 各 GB sender 的独立 RTP/传输
```

视频帧为 H.264/H.265 Annex-B access unit；AAC 为完整 ADTS frame，codec config 为 AudioSpecificConfig；时间戳用纳秒，payload 使用共享 buffer。WHIP 的 Opus 输入先转 AAC，因此 WHIP→WHEP 包含 Opus→AAC→Opus，不能称为 Opus 直通。

同 worker 的 sink 直接收到帧；跨 worker 按目标 worker 分组，由 `worker_sink_dispatcher` 使用有界队列转发。普通结束先交付已排队帧，再交付 end；溢出按既定策略结束慢组。网络写入序列化和在途 buffer 由 transport 持有，不再由各 UDP session 复制通用写队列。

依据：`media/core/media_stream.*`、`worker_sink_dispatcher.h`、`media/net/{tcp,udp}_transport.*`。协议级封装队列、HTTP-FLV 直接 HTTP 输出队列和 HLS 分片保留仍有独立职责，不能按通用 transport 队列一并删除。

## 当前阶段

媒体核心、共享派生输出、TCP/UDP 写队列和跨 worker 分发已收口；近期工作是定向正确性验证、已证明的局部精简和格式规范化。当前代码与已记录测试基线见 [current.md](current.md)，本文件不保存易过期的容量数字或自动生成后续开发任务。
