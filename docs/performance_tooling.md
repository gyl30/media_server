# 性能测试工具链

## 审查范围与版本

- BASE_HEAD：`b4965b8a928ed692492db2f176cd96f9a0759b78`。
- Codex 分支：`codex/performance-tooling`，独立工作区：
  `/var/tmp/media_server_tooling/codex-b4965b8-20261012/worktree`。
- Pi 工作区与报告只读；不覆盖其 smoke、构建、运行包或结果。
- 本文第一版是能力审计及实施计划，尚未开发或执行下述新工具。
- 本轮仅完善 benchmark、测试与部署文档，不修改生产媒体处理、鉴权或端口规则。

## 生产协议与 codec 边界

输入有 RTMP、RTSP publish TCP/UDP、RTSP pull TCP、WHIP、GB UDP/TCP active/TCP passive。
输出有 RTMP、RTSP TCP、HTTP-FLV、HLS、WHEP、GB UDP/TCP active/TCP passive。

RTSP UDP **播放不支持**：`media/rtsp/rtsp_play_session.cpp:317-336`
只接受 `RTSP_TRANSPORT_RTP_TCP`，其他请求返回 461。RTSP pull 同样固定 TCP，
见 `media/rtsp/rtsp_pull_session.cpp:178-194`。不能因为 UDP publish 存在就推断 UDP play/pull 可用。

标准 profile 为摄像机派生的 H.264 Baseline + AAC，视频必需、音频可选。
其他 profile 必须独立标识，不将合成 H.264/Opus 与固定摄像机文件视为相同负载。

- RTMP/RTSP/FLV 支持 H.264/H.265 与 AAC/Opus/G.711，
  见 `media/flv/flv_muxer.cpp:29-56`、`media/rtsp/rtsp_play_session.cpp:525-581`。
- HLS 显式跳过 Opus 音轨，保留受支持的视频及其他音轨，
  见 `media/hls/hls_segmenter.cpp:108-125`。不能将这种结果记为双轨 PASS。
- WHIP 接收 Opus 后转为 canonical AAC，见
  `media/webrtc/whip_media_receiver.cpp:53-99`；因此 WHIP 双轨源可以输出 HLS/GB。
- WHEP 的 AAC→Opus 是共享派生处理，见 `media/webrtc/whep_audio_egress.cpp:40-84`。
  SDP/profile/level 必须实际协商；H.265 等扩展 profile 的工具未验证时不得算 PASS。
- GB PS 要求一个视频、至多一个音频，音频支持 AAC/G.711，不接受 canonical Opus，
  见 `media/ps/mpeg_ps_output.cpp:25-49`。

### 输入 × 输出矩阵（审计起点）

以下针对标准 H.264/AAC canonical profile；WHIP 的线上音频为 Opus，入站转 AAC。
`VERIFIED` 仅指 Pi 的 1×1 基本媒体路径，不代表高并发工具已就绪。
`PARTIAL` 表示仅有可读性或字节增长证据。
`TOOL_MISSING` 表示生产支持但当前计划所需工具/授权/健康判定不完整，绝非生产不支持。

| 输入 | RTMP | RTSP TCP | FLV | HLS | WHEP | GB UDP | GB TCP active | GB TCP passive | RTSP UDP |
|---|---|---|---|---|---|---|---|---|---|
| RTMP | VERIFIED | VERIFIED | PARTIAL | PARTIAL | VERIFIED | TOOL_MISSING | TOOL_MISSING | TOOL_MISSING | UNSUPPORTED |
| RTSP publish TCP | SUPPORTED | SUPPORTED | PARTIAL | SUPPORTED | SUPPORTED | TOOL_MISSING | TOOL_MISSING | TOOL_MISSING | UNSUPPORTED |
| RTSP publish UDP | SUPPORTED | SUPPORTED | PARTIAL | SUPPORTED | SUPPORTED | TOOL_MISSING | TOOL_MISSING | TOOL_MISSING | UNSUPPORTED |
| RTSP pull TCP | TOOL_MISSING | TOOL_MISSING | TOOL_MISSING | TOOL_MISSING | TOOL_MISSING | TOOL_MISSING | TOOL_MISSING | TOOL_MISSING | UNSUPPORTED |
| WHIP | TOOL_MISSING | TOOL_MISSING | TOOL_MISSING | TOOL_MISSING | TOOL_MISSING | TOOL_MISSING | TOOL_MISSING | TOOL_MISSING | UNSUPPORTED |
| GB UDP receive | TOOL_MISSING | TOOL_MISSING | TOOL_MISSING | TOOL_MISSING | TOOL_MISSING | TOOL_MISSING | TOOL_MISSING | TOOL_MISSING | UNSUPPORTED |
| GB TCP active receive | TOOL_MISSING | TOOL_MISSING | TOOL_MISSING | TOOL_MISSING | TOOL_MISSING | TOOL_MISSING | TOOL_MISSING | TOOL_MISSING | UNSUPPORTED |
| GB TCP passive receive | TOOL_MISSING | TOOL_MISSING | TOOL_MISSING | TOOL_MISSING | TOOL_MISSING | TOOL_MISSING | TOOL_MISSING | TOOL_MISSING | UNSUPPORTED |

所有 N>1 的授权路径另须验证，不能从上表的单 viewer 推断。

## 当前工具证据与真实缺口

| 能力 | 当前实现与证据 | 结论与优先级 |
|---|---|---|
| 固定摄像机素材 | Pi 标准 fixture SHA256 `59c81471062a78e114205c96c03a99a1a3f432e50365bcd587edccd8ba029741`；`perf_smoke.py:214-226` 用 FFmpeg `-re -stream_loop -1 -c copy` | 可复用；保留 hash/profile。 |
| RTMP/RTSP 实时发布 | Pi smoke 已验证 TCP/UDP；既有 C++ RTSP publisher 是合成源 | 复用 FFmpeg 文件发布，不重写 RTSP publisher。 |
| WHIP 文件发布 | `bench/whip_publisher.cpp:215-240` 编码合成视频，现有参数不能指定独立发布 token | P0：独立 URL；P1：固定文件模式，必要的音频转换单独记录。 |
| RTSP pull 输入 | 控制入口已存在，基准未编排本地上游 | P1：在同一 namespace 内复用第二台媒体服务/固定文件源，不访问摄像机。 |
| GB 输入/输出 | `gb_network.py:29-52` 有三种建连方向；`gb_sender_fanout.py:444-463` 缺 TCP passive，且无 Bearer | P0：控制授权；P1：补齐方向与直接 RTP/PS 健康。 |
| 逐 viewer 一次性授权 | RTMP `rtmp_fanout.cpp:223-230`、RTSP `rtsp_fanout.cpp:209-213` 全 viewer 复用路径 | P0：每 viewer 凭据文件。单 viewer 可把 token 放入现有路径，不能将其说成协议无鉴权。 |
| WHEP 授权 | `whep_fanout.cpp:141-164` 已有独立 URL 文件，限制 sources=1；Python wrapper 未透传 | 复用 native 客户端，补多源及编排，不另写 PeerConnection。 |
| RTMP 首媒体 | `clients/rtmp_client.cpp:141-172` 过滤 `FLV_AVPACKET` | 该过滤不是序列头即媒体的已证 bug；需补音轨与持续增长门槛。 |
| RTSP 首媒体 | `clients/rtsp_client.cpp:37-47,169-186` 等两个固定 channel，无首媒体总超时 | P0：实际 SDP track 映射、video-only、有限超时。 |
| 双轨稳态健康 | RTMP/RTSP fanout 只将视频增长作为 progressing；总窗口增长无法排除中途停滞 | P0：按预期轨道检查每 viewer 周期增长，保留首次失败及阶段。 |
| FLV 健康 | `http_flv_fanout.py:44-74,142-155` 首 tag 后只数 socket bytes | P0：持续解析媒体 tag，区分音视频，不计 HTTP/header 为媒体。 |
| HLS 健康 | `hls_client.py:211-240,286-316` 只取最新段；Pi smoke `perf_smoke.py:130-145` 每次重下所有段 | P0：media-sequence/唯一分片持续前进，完整响应下载；重复段不可充当媒体推进。 |
| Pi 发布 smoke | `perf_smoke.py:238-240,291-294` 固定等待；`media_bytes=None` 自动通过 growing | PARTIAL：可读性证据成立，持续所有源媒体增长未验证。 |
| GB 健康 | `gb_sender_fanout.py:106-183` 有 RTP version/PT/SSRC/seq 计数；pair 用接收源 FLV 字节 | 复用解析与控制；补 PS 媒体进展、TCP passive；callback-only `gb_ps_fanout` 不是网络容量。 |
| 多源与混合 | `mixed_fanout.py:18-62` 硬编码路径和旧流名；只覆盖四种输出 | P0：在现有 clients 上统一授权与测量协调，补 FLV/GB/HLS 多源。 |
| 测量窗口 | native RTMP/RTSP/WHEP 已有全部 ready 后 warmup；不同进程没有共同窗口；Pi 在建立前就采样 | P0：跨客户端共同单调时间起点，发布/client/server 同窗；准备成本分开。 |
| 资源采样 | `fanout_support.py:13-81` 有 CPU、线程、RSS/PSS、FD、UDP queues/drops | 复用；补实际时间、TCP queues、namespace retrans/drops、publisher/client 和恢复快照。 |
| profiler | 测试规范 §9 已有 perf、BCC/bpftrace、jemalloc 命令 | 不开发新 profiler；记录缺依赖为 BLOCKED，不静默改变系统库。 |
| 部署 | `package_runtime.py:60-89` 打包 7 ELF、bench Python、fixture、ldd；未打包 smoke 的 tests/signaling_stub.py 依赖 | P0：新增工具与依赖纳入清单，manifest 覆盖脚本/hash/profile；真实 TEST 验证。 |
| 容量驱动 | `performance_baseline.py:44-49` 硬编码 workers=6，无健康容量分类 | P1：简单外层驱动 +500、100 粒度二分、H/F 复验及诊断档计划；不在本轮执行容量。 |
| churn/replacement/soak | `lifecycle_verify.py:233-499` 已有功能路径，但固定 6 workers/loopback/旧授权 | P1：复用流程，接入授权、地址、worker 参数与统一观测，不恢复媒体历史模型。 |

### Pi 证据的边界

Pi 报告目录 `docs/performance_reports/20261011T151927Z-b4965b8-2737042/`
记录 Stage 0 和 8 项 smoke PASS。8 项是 3 个输入 smoke 与 5 个输出 smoke，不是 8×8 矩阵。
本轮只读核对其 `manifest.json`、`README.md` 和新增 `bench/perf_smoke.py`。
该文件仍是 Pi 未跟踪的工作，不将其从原 worktree 移走或修改。

其 smoke 使用 `tests/signaling_stub.py`，验证 C++ 正常调用一次性 `/internal/verify`，
不是完整 Go source-generation 业务验收。打包器未包含该依赖，Pi 报告的增量部署步骤不能
替代新版本 package manifest 的可复现部署。

HLS 的 27.78 MB 下载证据包含同一 playlist 内旧分片的重复下载，不能作为 15 秒实际媒体
推进或吞吐；FLV 字节增长不能单独证明双轨增长。保留原 PASS，不改写原报告，增加更严格的新验证。

## 最小实施方案与验收

1. 扩展既有 native 客户端的逐观众凭据、预期轨道、超时与共同窗口接口。
   URL/token 必须独立；空缺/重复参数负对照失败。不关掉服务端授权。
2. 复用已验证的文件发布、SignalingStub 的一次性认领边界、HTTP/HLS/GB 接收逻辑，
   用一个小编排入口组合输入/输出。控制 API 必须发送 Bearer；受控测试授权服务须
   明确标识，并另用真实 Go 签发凭据做集成验证，不冒充 Go 全业务状态机。
3. WHIP 增加固定文件输入，不改生产协商；GB 与 RTSP pull 用 namespace 内的本地上游。
   单源、多源与混合负载共享 ready→warmup→measurement 时序。
4. 使用简单外层容量调度器调用相同 runner；仅可信服务端失稳建立 F 边界。
   客户端资源不足标 BLOCKED，归因不确定标 INCONCLUSIVE，不能缩小失败分母。
5. 每轮输出稳定 JSON、原始首次错误、实际 monotonic 窗口、各进程资源、fixture/binary/script hash、
   收尾状态。不在媒体热路径逐 viewer 打日志，不建立通用测试框架。
6. 在 Codex 自有 TEST namespace、`198.19.0.10/.11`、server `0-7`/client `8-15`、8 workers 下，
   验证 1×1、1×10、适量多源、混合、坏/重放 token、媒体中断、启动失败与清理。
   不运行最大容量搜索、超载或长期 soak；大负载仍为 NOT_RUN。

无需开发的功能：新媒体协议、RTSP UDP play/pull、关闭鉴权旁路、通用编排框架、
新的 profiler、全局资源恢复机制、生产端口重试、自动 reconnect，以及正式容量报告。

## 状态与交付门槛

统一结果使用 PASS、FAIL、BLOCKED、UNSUPPORTED、NOT_RUN、INCONCLUSIVE。
BLOCKED 的 reason 可以是 CLIENT_RESOURCE、REMOTE_ACCESS、PROFILE_TOOL 等。
FAIL 必须区分授权、建连、首媒体、稳态、上游、服务端与清理；容量调度器不能把所有 FAIL 当服务器上限。

工具参数、运行包、完整 TEST 验证与 Pi 命令将在实施后追加。

## Native 基础入口验证

RTMP/RTSP 新增 `--play-paths-file`，每行一个 `stream_id/token`，必须非空、互异、数量等于
`--viewers`。`--stream-name` 仍可用于单次诊断；正式多 viewer 必须使用独立凭据文件。
`--video-only` 指定视频单轨，默认要求音视频；`--ready-timeout` 默认 60 秒，覆盖解析地址、
连接、协商及首媒体；`--progress-interval` 默认 5 秒，完整窗口内预期轨道均需增长。

`tests/perf_tooling.py` 经实际 RED→GREEN 验证凭据入口、重复路径拒绝、连接拒绝清理、
无媒体 PLAY 超时、视频单轨、SDP 音视频顺序、稳态停滞以及 warmup 中断的有限退出。
SDP/停滞测试使用受控 TCP RTSP 对端和真实 ireader 客户端，不等同于真实素材解码验证。
其中旧工具在只发送约 0.25 秒媒体后仍返回 0，warmup 中断则超出 5 秒外层期限；修正后均返回失败。
连接拒绝还暴露了两个 benchmark 客户端对空 ireader 对象调用 destroy 的可达崩溃，已在客户端修复。

DEV 普通构建通过，相关 CTest 5/5 通过；此前全量 CTest 的其余 55 项通过，新工具开发过程中的
失败保留在 Codex 自有日志目录。后续完整工具及真实 TEST 验证尚未执行，不据此声明容量工具已完成。
