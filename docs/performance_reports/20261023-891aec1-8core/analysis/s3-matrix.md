## 3 全量协议矩阵

标注与任务要求的判定对应：**通过**（PASS）= 原生客户端 ready/progressing 全部满足、实时比例 ≥0.98，RTMP/RTSP/FLV/HLS 还经 ffprobe 6 s 解码确认音视频帧数；**失败**（FAIL）= 服务端功能问题（附缺陷编号）；**不支持**（UNSUPPORTED）= 服务端按设计拒绝；**工具缺失**（BLOCKED）= 测试工具无法产生该输入；**部分复现**（INCONCLUSIVE）= 重复轮次结果不一致；**未测** = 本轮没有运行，不计入 PASS。小负载矩阵每个输出 20 个 viewer（HLS 5 个），窗口 20～37 s，不是容量数据。

### 3.1 输入 × 输出（H.264 Constrained Baseline 720p30 + AAC LC 48 kHz）

数据：`raw/runs/v4-in-*`、`v5-in-whep_pull`、`d1-*`。WHIP 与 WHEP 拉流的上游媒体是 H.264 + Opus，服务端转成 AAC 后输出。

| 输入 \ 输出 | RTMP | RTSP TCP | HTTP-FLV | HLS | WHEP | GB UDP | GB TCP active | GB TCP passive |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| RTMP 发布 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 |
| RTSP TCP 发布 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 |
| RTSP UDP 发布 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 |
| WHIP | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 |
| RTMP 拉流 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 |
| RTSP 拉流 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 |
| WHEP 拉流 | **失败**（F1，时间线半速） | 通过 | **失败**（F1） | 通过¹ | 通过 | 通过¹ | 通过¹ | 通过¹ |
| GB UDP | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 |
| GB TCP active | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 |
| GB TCP passive | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 |

¹ 原生客户端按 RTP 时间戳（来自 pts）测得实时比例 1.002，HLS 可解码；但 TS/PS 中写入的 dts 同样受 F1 影响，PTS−DTS 持续增大，长时间播放的兼容性没有验证。

### 3.2 编码组合

数据：`raw/runs/v4-codec-*`、`raw/runs/b2-codec-*`。每格 20 个 viewer（HLS 5 个），RTMP/RTSP/FLV/HLS 用 ffprobe 解码 6 s，确认音视频帧数。夹具均为 720p30、GOP 2 s：H.265 Main，Opus 48 kHz 立体声，G.711 8 kHz 单声道。

标注：

- **通过**：全部 viewer 就绪且前进，实时比例 ≥0.98，解码出音视频。
- **F2**（FAIL）：HLS 视频可解码，但 G.711 音频被 ffprobe 识别为 `mp3` 或无法识别，解出 0 帧（缺陷 F2）。
- **仅视频**：HLS 按设计跳过 Opus 轨（`docs/protocols.md`），视频可解码。
- **不支持**：服务端按设计拒绝。
- **工具缺失**：测试工具无法产生该输入。

| 编码 | 输入 | RTMP | RTSP | HTTP-FLV | HLS | WHEP | GB UDP | GB TCP active | GB TCP passive |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| H.265 + AAC | RTMP 发布、RTSP TCP 发布、RTSP UDP 发布、RTMP 拉流、RTSP 拉流、GB TCP active、GB TCP passive | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 | 通过 |
| H.265 + AAC | GB UDP | 通过² | 通过² | 通过² | **2/4 轮失败（F6）**² | 通过² | 通过² | 通过² | 通过² |
| H.265 + AAC | WHIP | 工具缺失³ | | | | | | | |
| H.265 + AAC | WHEP 拉流 | 不支持⁴ | | | | | | | |
| H.264 + Opus | RTMP 发布¹、RTSP TCP 发布、RTSP UDP 发布、RTMP 拉流¹、RTSP 拉流¹ | 通过 | 通过 | 通过 | 仅视频 | 通过 | 不支持⁵ | 不支持⁵ | 不支持⁵ |
| H.264 + Opus | WHIP、WHEP 拉流 | 见 3.1（Opus 先转 AAC，全部输出可用） | | | | | | | |
| H.264 + Opus | GB 三种输入 | 不支持⁶ | | | | | | | |
| H.265 + Opus | RTMP 发布¹、RTSP TCP 发布 | 通过 | 通过 | 通过 | 仅视频 | 通过 | 不支持⁵ | 不支持⁵ | 不支持⁵ |
| H.264 + G.711A | RTMP 发布、RTSP UDP 发布、RTMP 拉流、RTSP 拉流、GB UDP、GB TCP active、GB TCP passive | 通过 | 通过 | 通过 | F2 | 通过 | 通过 | 通过 | 通过 |
| H.264 + G.711A | WHEP 拉流 | 不支持⁴ | | | | | | | |
| H.264 + G.711U | RTMP 发布、RTSP TCP 发布 | 通过 | 通过 | 通过 | F2 | 通过 | 通过 | 通过 | 通过 |
| H.265 + G.711A | RTMP 发布、RTSP TCP 发布 | 通过 | 通过 | 通过 | F2 | 通过 | 通过 | 通过 | 通过 |

注：

1. **RTMP Opus 发布**：ffmpeg n7.1.1 的 FLV 封装拒绝 Opus（`v4-codec-h264_opus-rtmp` 原判 BLOCKED）。RTMP/RTSP 拉流的上游实例也由 RTMP 推流供给，同样需要 n9.0；`b2-codec-h264_opus-rtsp_pull` 因任务脚本漏配 n9.0 而上游无轨道，已作废，以 `-n9` 重跑为准。经用户同意改用 ffmpeg n9.0.2，以 Enhanced RTMP FourCC `Opus` 发布并解码（第 9 节）。服务端 RTMP/HTTP-FLV 输出 Opus 也用 FourCC，需要 n9.0 的 ffprobe 才能解码。
2. **H.265 + AAC 经 GB UDP**：
   - `b2-codec-h265_aac-gb_udp` 和 `-r3`：HLS 首个分片 10 s 内未生成，runner 中止；两轮整轮 `UdpRcvbufErrors` 分别为 110、108。
   - `-r2`：全部 8 种输出通过；`-hlsonly`：只开 HLS，通过；这两轮丢包为 0。
   - 判为 F6（UDP 输入默认接收缓冲容不下 H.265 关键帧突发），不是 H.265 或 HLS 本身的问题；表中其他输出按 `-r2` 记为通过。
3. **WHIP H.265**：`bench/whip_publisher.cpp:236-237` 只接受 H.264 + Opus 夹具，服务端 WHIP 接收 H.265 的分支本轮未验证。
4. **WHEP 拉流**：offer 只含 H.264 与 Opus（`docs/protocols.md`）。H.265 源时上游返回 400（`whep pull offer rejected ... status 400`）；G.711 源时 answer 中 `audio false`，只拉到视频，源就绪探测要求双轨，因此失败。
5. **GB 输出**：PS sender 只支持 AAC/G.711 音频，Opus 源创建 sender 返回 500（F3），本轮这几格未运行 GB 输出；`v4-codec-h264_opus-rtsp` 中 GB 三组的 create 均失败，可作佐证。
6. **GB 输入**：GB PS receiver 只支持 AAC、G.711 音频（`docs/protocols.md`），工具也没有构造 Opus PS 流，按设计记为不支持。
