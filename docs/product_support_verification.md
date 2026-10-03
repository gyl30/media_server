# 产品能力与真实互操作验证

## 范围与证据

本阶段从 9200b0008d065e5bb922e11a8980af0e0e5c365c 开始，恢复最近 100 条历史和六份架构/验证文档后，只处理真实解码或时间戳证据支持的问题。中途继续执行的基线为 2a70363；最终生产修改为 50cfd8f，验证工具为 73a81b2。本报告对应提交可通过本文件的 Git 历史查询。

完整机器矩阵：[product_support.json](verification_results/product_support.json)，含 135 个方向/协议/媒体组合单元、156 条实际输入→输出解码路径、原始证据路径及 SHA256。原始证据位于 /tmp/media_server_product-9200b00 和 /tmp/media_server_product_current-2a70363-dD0La6，保留 RED、失败实验及环境限制，不用后来的 PASS 覆盖它们。

矩阵以**公共可注册 source generation 的内部媒体组合**为基准；WHIP/WHEP 的 wire codec 在表下注明。implemented 只说明当前实现接受该组合；tested 包括实际失败的尝试；real_protocol_tested 要求真实网络/协议；decode_verified 要求 FFmpeg 或 Chrome 实际解码推进，不能由 HTTP 201、connected 或 RTP 包数代替。missing 是缺少证据/合适客户端，不自动等于需要开发新产品功能。156 条矩阵路径使用无 B 帧 fixture；B 帧补充验证及首次 RTCP 同步限制单列于下，不把矩阵通过推广成任意时间戳形态均通过。

## 支持矩阵

图例：**D** = 真实协议及解码通过；**I** = 已实现但缺少该组合的真实解码；**L** = 实现有明确互操作限制；**N** = 当前公共产品拓扑/协商不支持。video-only 分别测试 H264/H265。audio-only 的内部组件测试不构成公共发布能力承诺。

### Publisher

| 输入 | H264 AAC | H265 AAC | H264 G711A | H264 G711U | H265 G711A | H265 G711U | H264 only | H265 only | audio only |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| RTMP | D | D | D | D | D | D | D | D | N |
| RTSP TCP | D | D | D | D | D | D | D | D | N |
| RTSP UDP | D | D | D | D | D | D | D | D | N |
| WHIP | D¹ | I¹ | N | N | N | N | I | I | N |
| GB UDP | D² | D² | D² | D² | D² | D² | D² | D² | N |
| GB TCP active receiver | D² | D² | D² | D² | D² | D² | D² | D² | N |
| GB TCP passive receiver | D² | D² | D² | D² | D² | D² | D² | D² | N |

1. WHIP 接收 **H264/H265 + 可选 Opus**，在输入边界把 Opus 转为 AAC；不是支持 wire AAC。真实 Chrome H264/Opus 发布后，FFmpeg 解码下游 H264/AAC。H265 和 video-only 实现存在，但本环境没有补齐真实发布端解码证据。
2. GB 的 D 来自实际 UDP/TCP sender→receiver→RTSP 解码。TCP sender active 配 receiver passive，sender passive 配 receiver active；JSON 按接收/发送角色分别归类。它证明本产品 RTP/PS 路径，不代表真实厂商摄像机、SIP 平台或非本机网络互操作。

### Player

| 输出 | H264 AAC | H265 AAC | H264 G711A | H264 G711U | H265 G711A | H265 G711U | H264 only | H265 only | audio only |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| RTMP | D | D | D | D | D | D | D | D | N |
| RTSP TCP | D | D | D | D | D | D | D | D | N |
| HTTP-FLV | D | D | D | D | D | D | D | D | N |
| HLS MPEG-TS | D | D | L³ | L³ | L³ | L³ | D | D | N |
| WHEP | D⁴ | I⁵ | D⁴ | D⁴ | I⁵ | I⁵ | D | I⁵ | N |
| GB UDP | D² | D² | D² | D² | D² | D² | D² | D² | N |
| GB TCP active sender | D² | D² | D² | D² | D² | D² | D² | D² | N |
| GB TCP passive sender | D² | D² | D² | D² | D² | D² | D² | D² | N |

3. HLS 的 G711 映射为 private TS stream type 0x90/0x91。H264/G711A→HLS 被真实执行，但 FFmpeg 不识别其音频；独立 FFmpeg mux/demux 同样不能把这个私有映射识别为可解码 G711，不能归因为服务端丢音频。其他三组 private-TS G711 只有实现证据，未补兼容播放器。没有擅自增加 G711→AAC 转码。
4. AAC 经 source/profile 共享 egress 转为 Opus；G711A/U 分别 PCMA/PCMU passthrough，要求 8 kHz mono。视频仍 passthrough。
5. 服务端 H265 协商和 packetizer 已实现；本机 Chrome 153 的收发 capability 均没有 H265，不能宣称浏览器 H265 解码通过，也不能把环境缺口记成服务端不支持。

RTSP 播放目前只提供 TCP interleaved，发布支持 TCP/UDP。公共输入要求支持的视频，不把内部 audio-only media_stream 测试当成新增产品入口。

### Opus 与格式边界

- H264/H265 在媒体层是完整 Annex-B AU，没有视频转码。
- AAC canonical 使用 ADTS，RTMP/RTSP/TS/PS 做协议封装转换；AAC→Opus 只在共享 WHEP 输出边界。
- RTSP canonical Opus **确实存在**：H264/Opus RTSP TCP→RTSP TCP 实测 8 秒，视频 161 条、音频 398 条完整时间戳记录，回退为 0。不能沿用“项目不接受任何 canonical Opus”的旧假设。
- canonical Opus→WHEP 不转码。当前规则要求 48 kHz、1/2 channels、空 config，以及与原流兼容的 offer：立体声需 stereo=1，maxaveragebitrate=510000；声明 maxplaybackrate 时须为 48000，maxptime 须缺省或 120。默认 Chrome offer 对该 stereo source 拒绝音频，视频仍解码；明确兼容 offer 后 H264/Opus 双轨解码通过。两次结果都保留。
- RTMP/HTTP-FLV 存在原生 Opus 代码路径，但 FFmpeg 7.1.1 在独立 FLV mux 阶段已拒绝 Opus，不能用它证明这两条路径互通；未扩大格式范围或修改 fallback。
- HLS、MPEG-PS 不接受 Opus。WHIP 的 Opus→AAC 与 WHEP 的 AAC→Opus 是不同方向的既有边界，不是新增的通用转码链。

## 真实客户端结果

| 客户端/链路 | 实际验证 |
| --- | --- |
| FFmpeg 7.1.1 RTMP、RTSP TCP/UDP publish | 3 种输入 × 2 种视频 × AAC/G711A/G711U/none，共 24 组，全部通过 |
| FFmpeg 输出解码 | 每组 RTMP/RTSP/HTTP-FLV、GB UDP/TCP 双角色；AAC/none 再测 HLS，共 156 条路径。每条 3 秒、至少 50 视频帧、正确音频 codec、PTS 回退 0、正常退出，无资源 guard 触发 |
| RTSP pull | 普通、ASan、UBSan：invalid URL 400，create 201，connected、tracks ready；下游 FFmpeg 双轨解码 5 秒持续推进；错误 identity DELETE 404，正常 DELETE 204，再 DELETE 404 |
| Chrome 153 WHEP AAC source | 60 秒：H264 framesDecoded 4→1801，Opus totalSamplesReceived 4800→2885280；ICE/DTLS connected，实际 SRTP 媒体推进；PLI 0 |
| Chrome WHEP G711A/U、video-only | 各 15 秒，H264、PCMA/PCMU 或仅视频实际解码推进；DELETE 204→404 |
| Chrome WHIP | 浏览器真实 H264/Opus encoder，经 Opus→AAC 后 FFmpeg 30 秒解码 600 视频帧，A/V PTS 回退 0，正常删除 |
| ffplay 4.4.2 | SDL dummy 输出下真实 RTSP TCP 解码约 9 秒：264 条视频、420 条音频记录；probe 明确停止直播进程，不以退出码 123 当媒体失败或正常 EOF |
| VLC 3.0.16 | HTTP-FLV 12 秒：libVLC decoded_video=621、decoded_audio=975，末段仍增长，corrupted/discontinuity/lost-pictures/lost-abuffers 均 0；这些是 libVLC 统计计数，不等同独立去重 AU 数 |
| VLC RTSP | 安装包没有 live555、拒绝 --rtsp-tcp，未取得 RTSP 解码证据 |
| OBS / 真实 GB 摄像机或平台 | OBS 未安装；没有获授权的设备/平台端点，未宣称通过 |

WHIP 浏览器默认发布在第一次 IDR 后没有按测试需要产生周期 IDR，晚加入 viewer 按既有自然关键帧策略等待，原始超时保留。验证 fixture 通过**发布端** pause/resume 产生下一自然 IDR 后完成解码；这不是默认配置下任意 late join 都可立即出画的承诺。没有添加服务端 PLI/FIR/关键帧请求、history 或 GOP replay。

原生 WHEP smoke 客户端在收到视频后尝试发送一份 PLI；会话继续推进。当前生产 handle_packet 对入站 SRTP/RTCP 采用忽略策略，没有反馈转发至 source。浏览器成功窗口的 PLI 计数为 0，不把它当作主动反馈恢复能力的证明。

ffplay 最初 probe 使用 -t 6 -autoexit 等待直播进程自然退出；实际已解码约 6 秒却继续等待网络输入，25 秒观察超时。后续 probe 改为固定观察窗后显式退出，不修改服务端。VLC 初次因缺 --rtsp-tcp 未能建立 instance，改测其环境支持的 HTTP-FLV；两份失败结果都保留。

## 确认并修复的问题

### H265 FLV / RTMP 互操作

RED：支持的 H265 source 在 RTSP 可解码，旧 FLV codec id 0x0c 经 RTMP/HTTP-FLV 无法被 FFmpeg 正常识别；独立 FFmpeg Enhanced FLV 能解码。根因是 wrapper 没有启用 vendored muxer 已有的 Enhanced RTMP 模式。

30c09f4 只在 H265 track 初始化时启用现有 API。新增 rtmp_hevc_decode CTest 修复前 RED、修复后 GREEN；本轮矩阵所有 H265 RTMP/HTTP-FLV 组合均通过。没有兼容层、新 codec、视频转码或 third 修改。

### RTSP SR 时钟随到包时间跳变

RED：连续 RTMP→RTSP 40 秒出现 2 次音频 PTS 回退；RTSP UDP→RTSP 60 秒出现 8 次；历史 600 秒结果为 67 次。捕获中 RTP 本身单调，SR 的 NTP/RTP 关联却有约 21 ms 跳变。仅把 SR 延后到 mux 返回仍失败。

根因与最小修复见 [稳定性验证](stability_verification.md)：50cfd8f 建立一个 session-wide PTS↔NTP 基准，把 SR 转到各 track RTP 时钟；报告在当前 mux 统计更新后生成。公开 SETUP/PLAY 定向测试用延迟到包稳定复现 RED/GREEN；600 秒真实解码回退 0。没有修改 RTP payload、媒体 PTS/DTS、网络队列、fanout 或 third。

### 测试工具误判

291de64、73a81b2 修复 FFmpeg 多线程日志交错时，把音频 PTS 归给视频的正则。只识别完整 n / pts / pts_time 行，不过滤真实时间戳回退。原始日志与修复前后计数保留；这是测试问题，不是生产解码修复。

### B 帧首次 RTCP 同步边界：保留严格检查失败

补充 FFmpeg 生成的 H264/H265 B=2、25 fps、AAC fixture，ffprobe 确认真实 PTS/DTS 重排序。RTMP、HTTP-FLV、HLS 六条解码路径均通过，窗口 5–6 秒，A/V PTS 回退 0；HTTP-FLV 首视频比首音频晚约 61 ms，与媒体的重排序时间轴相符。

RTSP 的严格单调断言仍为 **FAIL**：H264 连续 15 秒解码 375 帧、H265 6 秒解码 150 帧，FFmpeg 均正常退出，视频 PTS 无回退，音频各一次从首包 0 到约 −40 ms 的回退，此后持续单调。未过滤首帧、负 PTS 或失败结果。独立 FFmpeg→RTP→FFmpeg（不经过本服务）也观察到首 SR 建立跨轨同步时的一次音频回退。

对本服务 H264 RTSP 出口抓包，音视频各 7 份 SR 的 RTP/NTP 映射残差均小于 1 个 tick；首音频相对首视频为 −60.990 ms。首音频 RTP 在 frame 31，首次音频 SR 在 frame 33，客户端先按单轨起点输出首包，随后切到跨轨同步时间轴。该归因同时依据 [FFmpeg 7.1.1 RTP 时间戳处理](https://raw.githubusercontent.com/FFmpeg/FFmpeg/n7.1.1/libavformat/rtpdec.c) 和 [RTSP 跨轨 NTP 起点同步](https://raw.githubusercontent.com/FFmpeg/FFmpeg/n7.1.1/libavformat/rtsp.c)，以及独立对照；它与前述反复 SR 映射跳变不同。证据见 JSON 的 b_frame_verification，包含抓包 SHA256、公式和每份 SR 残差。

把服务端首 SR 基准改为 DTS 的临时实验仍出现同一首次回退，已撤回。当前证据不支持再修改服务端媒体时钟、丢弃首音频或增加等待缓存；这里记录客户端首次同步边界，不宣称 RTSP B 帧场景满足“从第一包起 PTS 永不回退”。

除上述已有闭环问题外，当前没有新的证据支持修改生产实现。

## 时间戳、生命周期与长期运行

| 检查 | 证据及边界 |
| --- | --- |
| 单 source RTSP UDP→RTSP TCP 600 秒 | 17,945 解码帧；完整视频/音频记录 17,947/25,840，回退 0；末 PTS 600.028/600.000023 秒，差 27.977 ms |
| 600 秒资源 | RSS 21,892–23,352 KiB，四段 median 23,186/23,168/23,204/23,188；VmSize 198,472–201,032 KiB；FD 31–32；6 threads；UDP drops 0 |
| 多协议/GB soak | 2445a3e 时执行 624.97 秒、20 source generations、20 GB UDP waves，包含 RTMP/RTSP/HTTP-FLV/HLS/WHEP viewers；PASS |
| soak 回收 | RSS 18,472→68,616 KiB、峰值 77,840；四段 median 63,756/67,534/67,400/67,776；VmSize 到 429,900 KiB 后稳定；FD 26→26；6 threads；平均 CPU 0.05285 cores；UDP drops 0 |
| 最新换源回归 | RTMP/RTSP/WHIP/GB UDP 各 3 代：旧 viewer EOF、HLS 新 generation URL、旧 ENDLIST/retention/404、新媒体继续，全部通过 |
| 已有更大生命周期覆盖 | [UDP 验证](udp_transport_verification.md) 的普通 120 次换代、ASan/UBSan 各 80 次；各类 300 viewer/WHIP/GB churn 与异常断开。是历史证据，不伪装成本次重新执行 |
| 时间转换边界 | media_lifecycle CTest 验证 RTMP 32-bit wrap、small correction、新 generation reset、ms/ns/90kHz 边界；HLS/PS/RTP/AAC/Opus 由真实封装解码补充 |

这不是多日生产 soak 或极端 33-bit PS/32-bit RTP wrap 的完整外部客户端证明。10 分钟稳定窗口与 retention/allocator high-water 不足以声称“永不泄漏”；这里也没有把涨后稳定误报为 leak。端口和 registry 回收由 DELETE/404、旧流消失、端口复用与 FD 恢复验证，没有新增永久遥测或伪造内部 session/port count。

早前 UDP 生命周期短窗口曾观察到内核 receive drops，详见原报告，不能因 socket 关闭后计数消失就写成累计 0。本次长解码/soak 观测为 0，尚无同条件可重复容量瓶颈证据，因此不改 socket buffer、write queue 或 syscall。

## 性能结论

沿用 [三轮性能基线](performance_baseline.md) 和 [UDP/fanout 对照](worker_sink_dispatcher_verification.md)。约 7.819 Mbit/s H264/AAC fixture、6 workers，WHEP 1000 viewers 三轮 30 秒 median：CPU 3.7602 cores、RSS 379,504 KiB、8.0275 Gbit/s、双轨首媒体 p50 502 ms，失败为 0。UDP 重构后的独立 20 秒 1000 点为 CPU 3.8184 cores、RSS 373,044 KiB；不同采样窗口不能随意互算性能变化。

RTSP/RTMP/HTTP-FLV 8 viewers 历史 median CPU 分别 0.0230/0.0170/0.0180 cores；HLS 基准只轮询最新 segment，其吞吐不是完整顺序解码吞吐。性能客户端计包和双轨首媒体，不等同浏览器全部解码。

**1000 是已验证负载点，不是最大容量。** 本阶段没有新的 profiler 热点或可重复 queue pressure，不重跑找上限，不做性能优化。长解码测试部分与其他独立端口回归并行，仅用于 correctness/资源观察，不冒充新性能基线。

## 构建与 Sanitizer

- 正常构建、RelWithDebInfo、独立 UBSan、ASan+UBSan 均覆盖当前生产修改；正常/UBSan/ASan CTest 各 10/10。
- RelWithDebInfo 实际为 -O2 -g -DNDEBUG -Werror，主二进制有 debug info、未 stripped。
- RTCP 修复后普通/UBSan/ASan 全套真实多协议 smoke 与 RTSP pull 均通过；ASan/UBSan 各核对 66 份 smoke/pull 日志，报告 0。预编译第三方并非全部插桩，不能据此宣称覆盖一切 C library 错误。
- 当前 ASan 使用已可用的 Boost 1.92 静态 ucontext/ASan 构建，未绕过 guard；旧的“ASan 被 guard 阻断”不再适用于此环境。普通/perf/独立 UBSan 构建仍使用其既有系统 Boost 1.90 配置，不冒称全部依赖已迁移静态 1.92，也未改 CMake 默认策略。
- TSan 的普通 fcontext 缺少可靠 fiber hooks，历史 clean 结果不作为 race-free 证明，不为此改架构。
- Go test/vet 通过，两个包均无测试文件；signaling 无修改。

## 保持冻结及后续边界

检查而未修改：transport/worker dispatcher、media_stream generation/registry、共享 PS、共享 AAC→Opus、HLS retention/lease、自然关键帧策略、port pool、source identity fencing。whole-stage 生产 diff 仅 H265 Enhanced FLV 模式和 RTSP SR 时钟修复；没有 third 或 signaling 修改。

下一阶段仍缺：授权 GB 摄像机/平台互操作、H265 WebRTC 客户端、WHIP H265/video-only、兼容 private-TS G711 的播放器、FLV Opus 外部客户端、OBS、可靠 fiber-aware TSan、多日/非本机运行。它们是明确验证边界，不据此主动扩 codec、转码、反馈或架构。

本阶段所有生产问题都已有 RED→根因→最小修复→GREEN→回归；其余限制保留原策略。当前没有待实施的已确认生产修复或性能优化任务。
