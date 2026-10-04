# GB28181 最小 Web 管理界面

## 基线与交付范围

BASE_HEAD：`9239dcaf26ef68a4e144b1aa951b6a04290a53a9`。

CODE_HEAD：`e8e03e6b0c257f57f6188caaec78578a07e2f022`。

FINAL_HEAD 是包含本报告的文档提交，用 `git log -1 --format=%H -- docs/gb28181_web_ui.md` 定位，避免在提交中循环引用自身 SHA。

本轮补齐已有 signaling MVP 的管理界面，继续使用 Go embed 静态资源和原生 HTML/CSS/JavaScript。没有前端框架、npm、构建链或新媒体协议。设备、registration、channel、live、ticket 的持久化与运行时归属保持原样；GB signaling/live/ticket 生产实现没有修改。

## 页面与操作

页面只有设备列表、设备详情/通道、共用播放器三层，另保留「RTSP 源」入口。首页默认展示设备名称、20 位编码及带文字的在线/离线圆点。离线设备仍可见。GB 页面不展示 SIP endpoint、SSRC、RTP 端口、stream_name、live UUID 或票据 URL。

添加设备弹窗校验编码与名称，成功关闭并刷新；400、409 和其他错误分别映射为中文输入错误、设备已存在、添加失败。详情提供返回、名称、编码、状态和删除。通道展示名称/编码、在线状态、取流状态；OFF 通道的播放按钮禁用。

| 操作 | 正式接口 | 前端行为 |
| --- | --- | --- |
| 添加/列表 | `POST/GET /api/devices` | 持久化设备列表，包含离线设备 |
| 详情/通道 | `GET /api/devices/{id}`、`GET /api/devices/{id}/channels` | 离线时不请求通道 |
| 播放 | `POST /api/devices/{id}/channels/{channel}/play` | 获取票据后立即 POST SDP |
| 关闭播放器 | `DELETE` 实际 WHEP `Location` | 只关闭当前 viewer |
| 停止设备取流 | `DELETE /api/lives/{live_id}` | 整路上游结束，所有 viewer 结束 |
| 删除设备 | `DELETE /api/devices/{id}` | 后端先清理 live，再删除设备 |

删除确认文案为「删除设备会停止该设备当前所有播放，是否继续？」；前端不先 DELETE live。成功关闭对应播放器并返回列表；502 显示「设备暂时无法删除，媒体资源清理未完成，请稍后重试。」，保留设备详情。添加、播放、停止、删除的 pending 操作禁用相关按钮；同通道只有一个本地 pending play。

RTSP 源仍支持添加、编辑、开始、停止、预览和删除。停止/删除当前源时也清理对应 viewer，不影响另一个正在播放的目标。

## Polling 与生命周期

设备及选中在线设备的通道每 2.5 秒刷新，不使用 SSE/WebSocket。请求采用 AbortController 与 epoch/选中设备身份检查；过期响应不能覆盖新选择。在线但 Catalog 尚未完成时显示「正在同步通道…」，随后没有通道则显示「未发现通道」。

设备 offline 或消失会结束其播放器，清空通道；重新 REGISTER 后恢复 Catalog，允许再次播放。跨浏览器停止 live 时，通道响应中的 live_id 与当前播放器身份不符会结束旧 viewer。正在进行的 play 不使用旧通道快照判断新 generation 已结束。

页面刷新只关闭 viewer，已有 GB live 可继续存在。详情显示「正在取流」，再次点击播放申请新票并复用 live，不增加 INVITE。没有 viewer refcount 或 idle auto-stop。

切换 channel、切换入口、返回列表、删除设备及 pagehide 均关闭本地 PC，清空 video.srcObject，并在已知 Location 时 DELETE WHEP resource。关闭先发起 DELETE，再立即清理本地，DELETE 最长等待 3 秒且失败也不保留本地 PC。取消后迟到的成功 offer 响应通过 generation fencing 清理返回的资源，不能复活旧播放器；迟到清理及 pagehide DELETE 使用 keepalive。浏览器直接被终止时 HTTP 清理仍属于尽力通知，未收到通知的远端资源由已有媒体生命周期回收，不增加后端 viewer 状态。

## WHEP 播放流程

沿用现有 `WHEPPreview`，关闭方法明确命名为 `closeViewer()`，整个取流停止操作使用 `stopLive()`。

1. 创建 RTCPeerConnection，添加 video/audio recvonly transceiver。
2. createOffer、setLocalDescription，等待 ICE gathering complete。
3. 调用正式 play 接口获取一次性 30 秒票据，立即消费 whep_url。
4. POST application/sdp，保存实际 Location，读取 SDP answer 并 setRemoteDescription。
5. ontrack 将实际到达的轨道加入同一 MediaStream；video-only 不等待不存在的 audio。
6. UI 显示「正在连接/正在播放/播放失败/已结束」。音频自动播放被拒绝时显示「点击播放音视频」，保持 unmuted。

GB WHEP 首次 404 或 409 只允许在 1.5 秒后申请一张新票重试一次，最多两张票，不重复消费旧 URL。真实首次取流可能在首帧前返回 409，且已消费票据；1 秒退避曾在测试中再次命中未就绪，1.5 秒退避复验通过。第二次失败直接显示中文错误，不自动无限点播。RTSP 源保留既有预览 URL 等待媒体就绪的行为，它不是 GB 一次性 ticket。

## 测试与证据

完整结果见 [gb28181_web_ui.json](verification_results/gb28181_web_ui.json)。原始运行目录：`/tmp/media_server_web_ui-9239dca/full-final-2`；日志、命令、截图和完整浏览器数据均保留在该目录。

完整 Chrome E2E 通过 22 项检查，持续时间 78.47 秒，无 pageerror。测试主要通过真实页面点击操作，未以 import 播放器模块替代 UI 验收。RTCPeerConnection 只加观察用记录，不替换网络或解码。

- UI 添加、中文输入/重复错误、离线详情、REGISTER/Catalog polling。
- OFF 通道禁用，离线设备不请求 channels，票据和 live UUID 不在页面正文出现。
- 点击播放与快速重复点击、解码推进、关闭 viewer 后 live 保留、重新播放复用。
- 三份独立浏览器上下文共享一路上游，刷新后不重复 INVITE。
- 停止整路 live 后全部 viewer 结束，新 generation 可再次播放。
- Expires:0 后离线/通道清空/播放器结束，重新 REGISTER 后恢复。
- 持有真实 ticket 响应 31 秒，WHEP 返回 404，再申请一张新票返回 201；两个失效票后无无限重试。
- 删除 pending 禁用，502 保留设备，WHEP DELETE 失败仍本地关闭。
- 返回列表后迟到 offer 被删除；播放中删除设备后 viewer/source 清理，重新 REGISTER 被 403 拒绝。
- RTSP 源 CRUD/start/stop/preview/restart/delete，以及同一播放器 H264/Opus 双轨。
- 真实 Chrome 音频 autoplay 拒绝后，通过明确按钮恢复 unmuted 播放。
- 1440 px 与 600 px 窗口截图；窄窗口没有页面水平溢出。

OFF channel、连续无效票、设备删除 502、WHEP DELETE 502 和迟到响应使用 Playwright HTTP 故障注入；30 秒过期、SIP、RTP、WHEP、解码、多 viewer、offline/re-register、删除与 RTSP 双轨使用实际服务。自动播放测试用 Chrome `document-user-activation-required` 策略，并通过 CDP 在无用户手势下触发该专门测试；普通验收操作仍为真实点击。

Chrome：`153.0.8010.52`。GB simulator 保持 H264 video-only。三 viewer 测量阶段 `INVITE=1`、`ACK=1`、`live_active=1`、`rtp_dropped=0`、`send_errors=0`。

| 样本 | codec | framesDecoded 前→后 | bytesReceived 前→后 |
| --- | --- | --- | --- |
| GB viewer | H264 | 3→51 | 17,997→202,127 |
| 共享 viewer 1 | H264 | 101→148 | 389,908→577,811 |
| 共享 viewer 2 | H264 | 26→73 | 108,002→295,905 |
| 共享 viewer 3 | H264 | 1→48 | 10,635→198,538 |
| RTSP 源 UI 视频 | H264 | 2→52 | 13,578→214,680 |

双轨 Opus 样本数 2,400→97,920，接收字节 902→19,980；全部样本 connectionState=connected、ICE gathering=complete、DTLS connected。音视频 fixture 经已有 RTMP source → RTSP pull → 同一 UI WHEP player，未改变 simulator 或媒体 codec 行为。

最终验证全部通过：`go test -count=1 ./...`、`go vet ./...`、`go test -race -count=1 ./...`、`cmake --build build -j12`、CTest 14/14。使用 Go 1.27.1，module 仍为 Go 1.26。CTest 在完整 E2E 所有进程退出后串行运行，未改变媒体端口池策略。C++ 生产代码零修改，本轮按要求没有重复 ASan/UBSan。

E2E 在生命周期阶段提交前执行；保存的所有静态资源及两个涉及 RTSP 请求的 Go 文件 SHA256 已逐项核对，与 CODE_HEAD 完全相同。

## 本轮发现与最小修正

1. 既有 Go 测试 peer 并发处理 ACK/BYE，BYE 先删除对话时迟到 ACK 可报不存在。修正仅限测试：只有已观察到 BYE 且错误是 ErrDialogDoesNotExists 才忽略，其他错误仍失败；定向 20 轮及最终全量/race 通过。
2. 现有 RTSP create 请求带 source_id，但 C++ 严格接口只接受 stream_id/stream_name/url/username/password，真实 UI start 返回 400。新增 Go 合约 RED 测试，随后仅在 Go 请求结构与构造处删除多余字段；C++ 不改，RED→GREEN 和真实 RTSP UI 均通过。
3. RTSP 源删除后本地 viewer 曾继续存在，真实 Chrome RED 已证明。前端停止/删除对应源时结束/关闭其 viewer，保留其他目标；GREEN 验证 PC 与 WHEP resource 释放。

相对 BASE_HEAD，生产文件新增 391 行、删除 323 行，净 +68：Web 净 +69，Go RTSP 请求净 -1，C++ 0。测试及文档不计入生产代码统计。

## MVP 边界

桌面优先、轻量中文 UI；无登录、RBAC、JWT、SSE/WebSocket、viewer count、live idle auto-stop。management API/UI 本身没有用户认证层，沿用受控管理网络假设。新增 GB 播放授权仅 WHEP；未新增 HLS/RTSP/HTTP-FLV UI 播放授权。GB live 仍仅 UDP，仍使用统一 SIP password。现有 RTSP 源管理/预览保留，不扩展其协议能力。

没有新增 Go ui_state、selected_channel、player_open 或 viewer registry。`/internal/live/*`、GB start/stop/preview 旧 API 未恢复，由既有 `TestRetiredGBControlRoutesAreAbsent` 持续保护。

## 播放器 correctness 复查

本次 BASE_HEAD：`f6b26807fd6db715706e0b24002486c30ce59749`；CODE_HEAD：`9e4a47f932362c6bafae4b2a7b2eaf6da5f4ee55`。仅修改 `whep.js` 和 `app.js`，生产代码净 +14 行，Go 后端与 C++ 零修改。

WebRTC `connectionState=failed` 表示当前 viewer 连接失败，显示「播放失败 / 连接中断，请重新播放」，使用已有 `webrtc_connection_failed`。复用小的资源清理函数，清空 current、关闭 PC/video，并尽力 DELETE WHEP resource。`closed` 不再推导业务结束。轨道结束、live 消失/换代及已有明确 source-end 仍显示「已结束 / 设备已离线或媒体已结束」，使用 `media_ended`。没有新增长期状态字段，也没有新增自动重连或重试。

关闭播放器按钮直接使用 `emit().canStop`，依据实际 current viewer ownership。失败/结束清理后按钮禁用；播放器的停止取流按钮仍从 current.liveID 推导，viewer 消失后隐藏。当前 live 仍可从通道行停止，不保存旧 live_id。

WHEP POST 等待审计：GB [代理](../signaling/play_http.go#L81) 设置读/写 deadline 为 media client timeout 的一倍/两倍；[client](../signaling/media_server_http.go#L56) 默认 timeout 为 3 秒，即默认读 3 秒、写 6 秒，上游 POST/响应体受同一 client timeout 限制。RTSP 预览直连 C++，[POST handler](../media/http/whep_http.cpp#L73) 同步查流、解析 SDP、创建 session 并返回 answer，[startup](../media/webrtc/whep_session.cpp#L165) 不等待 ICE/DTLS/媒体建立。[HTTP session](../media/http/http_session.cpp#L36) 已设置 Beast 30 秒 expiry；核对当前 Boost 1.92 的 basic_stream 文档，它覆盖随后异步读和响应写，直到被重设。该 I/O deadline 不抢占同步 C++ 执行，但没有发现等待媒体建立的阻塞路径或实际超限 RED；既有 RTSP 409 就绪重试不等于单个 POST 悬挂。因此未新增浏览器 timeout，保留迟到 201 的资源清理及 GB 单次 fresh-ticket 重试规则。

现有 Browser E2E 增加真实已连接、已解码 PC 的 failed 事件边界注入，并观察 emit 时实际 current 与按钮的关系。RED 复现错误的 ended 文案与仍可点击的关闭按钮；GREEN 完整 24 项通过（82.20 秒），覆盖 idle/preparing/streaming/failed/ended/stopping ownership、真实 live DELETE/Expires:0、正常 viewer close。失败后 live 保留，RTP 计数 615→768；重播申请新票并复用 live，INVITE 1→1。原生 PC、媒体网络及解码保持真实执行。完整证据：`/tmp/media_server_player_followup-f6b2680/full-final/result.json`，摘要追加在 [验证 JSON](verification_results/gb28181_web_ui.json) 的 `player_lifecycle_followup`。

本次 `go test -count=1 ./...`、`go vet ./...`、`go test -race -count=1 ./...`、normal build 与 CTest 14/14 全部通过；CTest 在 E2E 进程退出后执行。静态资源 SHA256 与 CODE_HEAD 核对一致。C++ 未修改，本次按要求没有重跑 ASan/UBSan。
