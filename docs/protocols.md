# 协议与媒体边界

以下是当前源码实现的方向和传输方式。codec 能进入内部表示，不等于所有输出、浏览器或设备都支持该组合。

## 接入与输出

| 协议 | 输入 | 输出 |
| --- | --- | --- |
| RTMP | TCP Publish；主动 TCP Pull | TCP Play |
| RTSP | TCP interleaved / UDP Publish；主动 TCP interleaved Pull | TCP interleaved Play |
| WebRTC | WHIP；主动 WHEP Pull | WHEP |
| GB28181 实时媒体 | PS-RTP，UDP / TCP active / TCP passive receiver | PS-RTP，UDP / TCP active / TCP passive sender |
| HTTP | 控制；有限历史 PS 消费入口 | HTTP-FLV、HLS MPEG-TS、有限历史 PS |
| GB 音频上传 | WebSocket PCMA | 独立 GB RTP 音频发送 |

GB UDP sender 绑定相邻的 RTP/RTCP 本地端口，只有请求带 `remote_rtcp_port` 时才发送 SR。接收端按惯例会向 RTP 源端口 + 1 回 RR，sender 读出后丢弃，不据此调整发送，也避免它们积压在内核接收队列里。

RTSP Play/Pull 没有 UDP 模式。主动拉流由 `POST /pull/create` 按 URL scheme 选择 RTSP、RTMP 或 HTTP WHEP；WHEP Pull 当前只接受 `http://`。RTMP URL 需要 `/app/stream`；RTSP 凭据单独传 username/password；WHEP 凭据使用 token。源码：[`pull_http.cpp`](../media/http/pull_http.cpp)。

## 授权 URL

发布、播放 URL 由 [信令 API](signaling-api.md) 签发。下面的占位符表示返回值，不是可自行反复使用的固定流名。

| 用途 | URL 路径 |
| --- | --- |
| RTMP 发布 | `rtmp://host:1935/live/{publish_token}` |
| RTSP 发布 | `rtsp://host:8554/{publish_token}` |
| WHIP 发布 | `POST /publish/whip/{publish_token}` |
| RTMP / RTSP 播放 | `/{stream_id}/{play_token}` |
| HTTP-FLV | `GET /{stream_id}/{play_token}.flv` |
| HLS 首次入口 | `GET /play/hls/{stream_id}/{play_token}/index.m3u8` |
| WHEP 播放 | `POST /play/whep/{stream_id}/{play_token}` |
| GB 历史播放/下载 | `GET /play/gb28181/{stream_id}/{play_token}.ps` |
| GB 麦克风上传 | WebSocket `/publish/gb28181/audio/{session_id}/{upload_token}` |

每张播放票据只可成功消费一次；同一响应中的不同协议 URL 共用该票据。每个 viewer、重试或切换协议都应重新申请。HLS 首次认证后返回 307，后续使用返回的独立 session URL。历史 PS 使用 HTTP/1.1 chunked、`Content-Type: video/mpeg`，不是普通实时 FLV。

WHIP/WHEP 使用 `Content-Type: application/sdp` 提交完整 offer，成功返回 201、answer 和 `Location`。关闭时 DELETE 该资源；已移除资源返回 404。服务端采用 ICE-Lite、DTLS/SRTP，一次完整 SDP 协商，不提供 PATCH/Trickle ICE、ICE restart 或自动重连。认证后的 nomination 可更新媒体端点，普通 ICE check 不直接切换目标。

## codec 处理

- RTMP/FLV 与 RTSP 路径实现 H.264/H.265、AAC、Opus、G.711 A/U 的解析或封装分支；参数仍需满足对应协议检查。RTSP Opus 为 48 kHz、1/2 声道，G.711 为 8 kHz 单声道。
- WHIP 接收 H.264/H.265 与可选 Opus；Opus 先转 AAC 再进入内部流，同时保留只含视频和原始 Opus 的伴随流。WHEP 按 SDP 和源轨道匹配输出：WHIP/WHEP 拉流源在观众 offer 满足 Opus 直通条件（立体声源要求 `stereo=1`，`maxplaybackrate`、`maxaveragebitrate`、`maxptime` 不限制）时直接转发原始 Opus；不满足时与其他 AAC 源一样经共享 egress 转 Opus。转码输出时间线按编码器报告的 initial_padding 提前，抵消编码器延迟（libfdk_aac 2048、libopus 312 样本）；补偿后时间戳为负的帧丢弃。
- WHEP 发出的 RTCP SR 按媒体时间线填写 NTP：会话首帧到达时的墙钟减其 pts 作为起点，SR 的 NTP 为起点加该 RTP 时间戳对应的 pts。浏览器按 SR 对齐音视频，因此音画关系与源时间戳一致，不受两条轨在服务端内部投递延迟差的影响。源码：[`webrtc_packetizer.cpp`](../media/webrtc/webrtc_packetizer.cpp)。
- 主动 WHEP Pull 的 offer/answer 范围是 H.264 与 Opus，不能从 WHIP 的 H.265 分支推导出 H.265 Pull。
- 主动 WHEP Pull 只向 answer 中与本端同地址族的 candidate 直接发起连通性检查，不使用 STUN/TURN；上游 candidate 必须能从媒体服务直接到达。源码：[`whep_pull_session.cpp`](../media/webrtc/whep_pull_session.cpp)。
- GB PS receiver 收集单路视频和可选音频后注册固定轨道，支持 H.264/H.265、AAC、G.711 A/U；PS sender 同样要求一路视频、至多一路支持的音频。TCP PS-RTP 使用两字节长度前缀。
- HLS 使用 MPEG-TS 分片，支持 H.264/H.265 和 AAC。Opus 与 G.711 没有通用播放器支持的 TS stream type，HLS 跳过这类音轨，只输出视频。
- Qt 客户端视频只接受 H.264，音频接受 AAC、PCMA、PCMU；不因服务器支持 H.265 就宣称桌面支持 H.265。

具体校验位于 `media/rtmp/rtmp_publish_session.cpp`、`media/rtsp/rtsp_play_session.cpp`、`media/webrtc/webrtc_sdp.cpp`、`media/webrtc/whip_media_receiver.cpp`、`media/flv/flv_tag_output.cpp`、`media/hls/hls_segmenter.cpp` 和 `media/ps/mpeg_ps_output.cpp`。

## 媒体控制 API

这些接口由信令或受控工具调用。配置 `--control-token` 时要求 Bearer；为空时只接受回环来源。入口和鉴权在 [`http_session.cpp`](../media/http/http_session.cpp)。

| 方法 | 路径 | 职责 |
| --- | --- | --- |
| GET | `/receivers` | 返回 receiver 的 `stream_id` 与媒体 `ready` |
| POST | `/receivers/delete` | 按 `stream_id` 提交停止接收 |
| POST | `/pull/create` | 创建 RTSP/RTMP/WHEP 主动源 |
| POST | `/gb28181/receiver/create`、`/gb28181/receiver/update` | 创建实时 PS-RTP receiver；更新 SSRC |
| POST | `/gb28181/receiver/playback/create` | 创建有限 TCP passive PS receiver |
| POST | `/gb28181/sender/create`、`/gb28181/sender/delete` | 管理独立 PS-RTP sender |
| POST | `/gb28181/audio/create`、`configure`、`status`、`delete` | 管理音频会话及控制租约；后三项沿用同一路径前缀 |

请求体为 JSON 对象，未知字段、重复字段、类型或范围不符都返回 400 和 `{"error":"..."}`；Go 信令的解码器同样拒绝未知字段，重复字段不会在两侧被解释成不同的值。回归用例见 [`regression/control_api.py`](../regression/control_api.py)。

创建 receiver 成功不等于轨道或首帧已到达。receiver 删除和 SSRC 更新的 204 表示已提交操作，不是跨 worker 资源释放完成通知。GB SIP 控制由 Go 服务处理，媒体服务不承担设备注册或 SIP dialog 管理。
