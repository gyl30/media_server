# 全协议时序、状态与生命周期精简审计

## 一、总体结论

当前基础设施与协议职责已经基本收口，没有发现值得再次开展全仓重构的结构性冗余。RTSP、RTMP、GB28181、HTTP-FLV、HLS、WHIP/WHEP 的主要状态分别表达协议阶段、固定轨道、源代际、异步操作或资源归属；不能只因为资源指针存在就删除这些状态。

本轮正式候选共 3 项：**REMOVE 2、SIMPLIFY 0、VERIFY 1；P0 0、P1 0、P2 3**。优先级只统计第五部分的候选，不给 KEEP/NO_CHANGE 或测试覆盖缺口强行定级。

- 可以直接实施：TCP transport 两处入口已保证回调存在、且中间没有挂起或用户代码的重复判空。收益很小，只减少分支，不删除字段、不改变网络策略。
- 必须保留未验证结论：RTSP Pull 关闭后成功读取继续推进 DESCRIBE/SETUP 的窗口。此前专项已观察到短暂媒体登记，但未证明持久残留或业务损害；本轮没有新增复现，不能将它升级为生产缺陷或再次追加防护。
- 没有新确认的严重生命周期缺陷。此结论限于源码、历史与现有测试覆盖分析，不是对所有异步排列的运行期证明。
- 必须保留：PLAY 阶段、RTMP Play.Start 与初始化包顺序、自然关键帧门槛、Pull 的取消和 identity 检查、迟到 PLAY 判空、PS topology、HLS generation、ICE/DTLS/SRTP、worker cancellation 与 shutdown subscription。

最有价值的方向是维持已有边界，并在真实需求或可复现后果出现时补充定向验证。没有五项值得立即修改的生产建议，不凑满数量。建议顺序为 R1、R2；V1 仅保留观察记录，其余协议当前保持现状。

## 二、审查基线

| 项目 | 实际值 |
|---|---|
| BASE_HEAD | `c2e7f2700794c36ede18e6c564d53deda23fc1ab` |
| 分支 | `main` |
| 工作区 | `/home/gyl/example/media_server` |
| 审查日期 | 2026-10-12，Asia/Shanghai |
| 起始工作区 | clean |
| 实施权限 | 用户最新授权直接使用主仓库；本 Goal 只写、提交本报告，不修改代码，不 push |
| 并行工作区 | Claude detached `67792f9`、Pi detached `b4965b8` 均未修改；未操作 TEST 服务、namespace 或运行包 |

已阅读项目规则、`docs/project_decisions.md`、`docs/development_conventions.md`、`docs/media_core_architecture.md`、`docs/startup_coroutine_audit.md`、`docs/webrtc_transport_audit.md`、`docs/system_validation.md`，并回顾最近约 100 个提交及下列关键差异。

| 历史提交 | 本轮核对的取舍 |
|---|---|
| `3af9496`、`a753d54` | Pull 的迟到 DNS 身份检查与迟到 PLAY 资源保护是实际修复，不能撤销 |
| `468c988`、`a915cbb` | 父子回调保活、逐层关闭、RTMP 内部初始化进入 owner 协程，解决真实启动/关闭时序 |
| `3dc1603`、`5023b2c`、`87e18a0` | WHIP/WHEP 外部协商、一次 startup、授权前移；旧 activate/阶段包装已经删除 |
| `1d60c93`、`7ca4a9b` | ANNOUNCE SDP 与 Pull URL 配置已经避免重复解析 |
| `042c984`、`d84e0b4`、`16ec823` | 已删 RTSP closed、重复关键帧字段、WHEP shutdown_requested 等；保留真实 PLAY 与资源 fence |
| `b113b83`、`87a686b` | PS topology 已精简；HLS Opus 跳过与内部 audio-only 分片有明确测试 |
| `2a863a4`、`e4187c3`、`a1f2f20` | TCP/UDP stopped 已删；回调和 socket 表达终态；高水位一次性通知由 exchange 表达 |
| `af82c36` | HLS playlist 等待已经改为现有协程中的局部 deadline/async_wait |
| `619e53e`、`f0f3099`、`ee40b5d`、`f1a6d49` | 播放授权前置，特别是 HLS 未授权请求不创建 segmenter/session |
| `9cd85d3`、`80ba127`、`4abaa83`、`c2e7f27` | 取消日志过滤、RTSP UDP/GB UDP 特殊错误处理已收口，本轮不重复提出 |

历史文档是取舍依据，当前源码是实现依据。旧文档提到的 WHEP 原子关闭字段、activate、F4 待修等不再描述当前代码。没有找到单独解释 TCP 两处内层判空必须保留的历史决定；第五部分以当前不变量与引入入口保护的差异为依据。

范围包含所有上述协议的生产入口、session、共享媒体/网络组件、相关 CMake 测试注册以及信令授权、GB 点播、RTSP 拉流管理与对账。未逐行审查 `third/**` 内部实现、浏览器播放器与 bench 热路径；这些不在本轮实施范围。ireader 的调用契约通过生产调用点与已有真实 TCP 测试交叉核对。本轮没有执行 CTest、Go、Sanitizer 或真实媒体集成，因此没有新的运行 PASS/RED/GREEN 结论。

## 三、全协议实际时序与所有权

### 3.1 RTSP 发布

`accept → owner 协程 → ANNOUNCE/SDP/授权 → SETUP TCP 或 UDP → RECORD → RTP/RTCP → TEARDOWN、断连、错误或外部删除`。

入口 `media/rtsp/rtsp_server_connection.cpp:36` 启动 owner 协程；`:42` 安装 TCP 写错误回调并初始化协议对象。父连接持有 transport、publish/play 子对象，协程和回调在有效期间保活父对象。

ANNOUNCE 在 `media/rtsp/rtsp_server_connection.cpp:281` 选择发布角色，`media/rtsp/rtsp_publish_session.cpp:75` 解析、保存轨道描述；授权失败不进入正式媒体注册。SETUP `:164` 校验 Session、轨道与传输，TCP 在 `:214` 准备 demux；UDP 子会话按轨道分配端口并初始化 socket。部分成功后的失败沿父关闭路径清理，不能因为 startup 返回 false 就认为资源从未分配。

RECORD `media/rtsp/rtsp_publish_session.cpp:242` 检查已 SETUP 轨道再开始记录；`media/rtsp/rtsp_publish_media.cpp:65` 固定轨道并登记媒体源。SETUP 完成和媒体已经发布是两个事实。

父 `media/rtsp/rtsp_server_connection.cpp:417` 移除接收身份、关闭并释放子会话、再异步关闭 transport。子 `media/rtsp/rtsp_publish_session.cpp:361` 清理 media、timer、UDP 子对象和 handlers；UDP `media/rtsp/rtsp_publish_udp_session.cpp:254` 清空父通知/输入 handler 后关闭两类 transport 并释放端口。关闭通知经 handler 的 move 只消费一次。不能把 child shutdown 和 parent shutdown 机械合并，它们释放不同资源，且 owner 队列顺序有实际意义。

正常结束、错误结束和 worker stop 共享现有释放机制；没有 RECORD started_。重复 SETUP、错误 Session、提前 RECORD 的检查属于输入协议，不是一次内部 startup 的防重复逻辑。

### 3.2 RTSP 播放

`DESCRIBE/查源/授权/prepare → SETUP interleaved → PLAY 响应 → playing/订阅 → 自然关键帧 → RTP/RTCP → TEARDOWN、源结束或错误`。

`media/rtsp/rtsp_server_connection.cpp:366` 处理授权与子对象准备；`media/rtsp/rtsp_play_session.cpp:234`、`:284`、`:359` 分别处理真实 DESCRIBE、SETUP、PLAY 阶段。当前输出路径是 TCP interleaved，不因为输入支持 UDP 就假设播放也支持同一模式。

`playing_` 仅在 PLAY 接受后开启，stream_ 在 DESCRIBE 阶段就存在，二者不能互换。PLAY 回复先于订阅后的媒体，等待视频自然关键帧。`media/rtsp/rtsp_play_session.cpp:67` 同时用 playing_ 与 keyframe gate 决定发送；RTCP 同步锚点用于多轨时钟，不能从轨道配置推导。

`media/rtsp/rtsp_play_session.cpp:206` 在 owner 清除 playing、解除订阅、释放 stream/muxer、清 handlers；父关闭释放 TCP。源 generation 的对象身份检查保留，不能仅比较 stream name。PLAY 内部创建与对外协议 PLAY 不是可合并的重复启动阶段。

### 3.3 RTSP Pull

`HTTP 参数解析 → registry 登记 → startup/协程 → DNS → connect → DESCRIBE → SETUP → 创建 media → 发 PLAY/安排 RTCP → RTP 完整轨道注册 → 关闭`。

`media/http/rtsp_pull_http.cpp:154` 解析控制请求，`:171` 创建并登记 session 后调用 startup，201 表示创建提交，不等待首媒体。`media/rtsp/rtsp_pull_session.cpp:231` 在指定 worker resolve/connect，`:235`、`:248` 在恢复后检查 cancellation 与 registry identity；成功完成不等于没有取消，这些检查不可删除。

transport 写回调持有父 session；C client 由协程 `:281` 创建，`:294` 在 read 循环退出后销毁。不能在 safe_shutdown 中立即销毁 C client，使仍在栈上的同步 C 回调悬空。

`media/rtsp/rtsp_pull_session.cpp:323` 的 DESCRIBE 回调发 SETUP；`:329` 最后 SETUP 回调整理描述，在 `:366` 创建 media，`:373` 提交 PLAY，提交返回 0 后 `:376` 安排 RTCP。**RTCP timer 的启动不等待 PLAY 响应**。媒体源注册由 `media/rtsp/rtsp_pull_media.cpp:222` 的固定轨道就绪决定，不等价于完成 HTTP 创建或仅收到 SDP。

主动关闭 `media/rtsp/rtsp_pull_session.cpp:147` 先移除接收身份、清理 media，再取消 timer/resolver/connect socket，并投递 transport 子关闭。late PLAY `:215` 使用已有 media_ 事实返回 -1，不新增 closed。迟到成功 DESCRIBE/SETUP 的证据边界见 V1；本轮不将已接受的短暂推进改写成确定泄漏。

### 3.4 RTMP 发布与播放

`owner 协程初始化 → handshake/connect → publish 或 play → codec config/metadata → 媒体 → 断连、源结束、错误或删除`。

`media/rtmp/rtmp_session.cpp:32` 的 startup 只 spawn；`:38` 内部初始化协议对象和回调。publish/play 子资源表达选定角色，不能额外加同义 role/started。发布 `:247` 授权后创建发布子对象并登记 receiver；子 `media/rtmp/rtmp_publish_session.cpp:27` 初始化 demux，轨道由配置和元数据逐步确定。

播放 `media/rtmp/rtmp_session.cpp:188` 查源、授权、prepare；`:237` 发 Play.Start，`:238` 才执行子 startup。`media/rtmp/rtmp_play_session.cpp:31` 的准备与 `:47` 的启动承担编码配置暂存和开始发送两个阶段。初始配置不能提前越过 Play.Start，也不能用初始化包 vector 是否为空推断阶段。

父 `media/rtmp/rtmp_session.cpp:279` 关闭子对象、timer 和 transport；C context 在 run 退出时销毁。输出回调仍需检查 context/transport（`:214`），迟到 frame 仍需子 stream fence（`media/rtmp/rtmp_play_session.cpp:82`）。网络、C 协议和媒体订阅分别持有必要引用，删除其中一层会改变生命周期。

### 3.5 GB28181 收发

控制入口 `media/http/gb28181_http.cpp:79` 创建 receiver，`:127` 创建 sender。登记、启动失败清理和异步 TCP 建立必须分别解释；201 不保证 active TCP connect 或媒体 ready。SIP dialog 在 signaling，RTP 资源在 C++，不将两者合并为同步控制事务。

- **UDP receiver**：`media/gb28181/gb28181_udp_receiver_session.cpp:36` 初始化 RTP/PS、分配成对端口与 transport；`:119` 成功读取即刷新 idle，合法 payload/SSRC 后才记录远端。RTCP 与 RTP endpoint 各有意义；`:217` 关闭 registry、timer、transport、receiver、端口。
- **TCP active/passive receiver**：`media/gb28181/gb28181_tcp_receiver_session.cpp:26`、`:43` 准备 connect 或 listener；`:74` 建立、检查取消，再接收长度前缀 RTP。`:160` 清理连接及解复用。accept 和 connect 是真实不同操作，不能只为共用 startup 外观再建框架。
- **UDP sender**：`media/gb28181/gb28181_udp_sender_session.cpp:43` 准备 RTP/RTCP、port、subscription 与 RTP sender。`:164` 首次实际 RTP 发送启动 RTCP reporting，`:131` 的 timer 和写完成推进下一轮。`:189` 先移空 stream_ 表示终态，再移除身份、清 child/transport/timer/port/RTCP。
- **TCP active/passive sender**：`media/gb28181/gb28181_tcp_sender_session.cpp:21`、`:37` 准备端点或 listener，`:48` 建立后创建 transport；`:64` 的取消和 stream fence 不等价于 connect 返回 error。`:133` 清理 sender、listener、transport 与注册身份。
- **PS topology**：`media/gb28181/gb28181_rtp_receiver.cpp:142` 收集 PSM 直到 finish；`:193` 应用完整 topology；`:237` 消费待应用信息，`:299` 在真实 codec config 齐全后注册固定媒体。codec、track 与 PS stream index 分别表示编码类型、初始化参数和 demux 通道。
- **共享 PS/RTP 输出**：`media/ps/mpeg_ps_output.cpp:25` 在 source owner 初始化并订阅；`:100` 等自然关键帧后 mux。`media/gb28181/gb28181_rtp_sender.cpp:27` 跨 worker 取得共享 PS，再投递 sender worker 注册 sink；`:118` 清 handler、订阅和 packetizer。跨 worker source/target 两阶段归属不可抹掉。

媒体结束与外部删除最终由 stream_、sender_、port 和 identity 清理；重复调用不应删除新 generation。GB UDP idle 行为、异步 SSRC/delete 204 是既定取舍，本轮不重新讨论。

### 3.6 HTTP-FLV

`HTTP GET → URL/源/授权 → writer/muxer/FLV Header → 订阅 → 自然关键帧 → HTTP chunk 串行输出 → 源结束、读写错误或关闭`。

`media/http/http_flv_session.cpp:87` 先校验与授权再创建输出资源。媒体回调在会话 worker，`:188` 入队 bounded HTTP chunk 数据，`:210` 的协程串行写入，`:238` 用 source_ 和 keyframe gate 保护媒体处理。这是 HTTP chunk 生命周期和背压队列，不能直接当作重复 TCP transport queue 删除。

`:300` 关闭 source/writer/muxer/socket；**不在该处清空 output_queue_**。在途 async_write 引用 queue front，`:219` 只在完成后错误分支清 queue。为了“关闭立即释放内存”提前清 queue 会破坏 buffer lifetime。4 MiB 字节计数和 queue 并非同一事实。

### 3.7 HLS

`一次性播放 URL → 查源/授权认领 → 共享 segmenter → 独立 secret 播放 session → playlist 等待 → segment 下载/续期 → viewer 过期或源结束后保留窗口`。

`media/http/hls_http_session.cpp:74` 入口解析；`:100` 附近的授权先于 get_or_create/session 创建（入口完整顺序见 `:74` 至 `:113`）。已授权 session 后续请求使用 secret，不能重新消费原 token。`media/hls/hls.cpp:72` 同名源比较对象身份，generation 替换关闭旧 segmenter，再创建新实例；旧 viewer 仍持有旧实例，不混合分片。

`media/hls/hls_segmenter.cpp:29` 处理源 frame，`:89` 结束 mux 并记录 ended_at；playlist/segment 读取受 mutex 保护。源在 source worker，HTTP reader 可以在其他 worker，锁不是重复 owner mutex。

`media/http/hls_http_session.cpp:141` 使用已有 yield、局部 10 秒 deadline 与短 timer 等待 playlist，无递归回调链；`:229` 响应保活 self、response 和 segment buffer。`media/hls/hls_play_session.cpp:33` 建 secret registry/owner subscription/timer，`:62` find 时更新时间，`:84` 判断 30 秒活动过期。源结束保留与 viewer idle 是不同期限，不能统一 timer 或删 ended_at。

### 3.8 WHIP / WHEP

`HTTP SDP 基础检查 → 一次性授权 → UDP/port/证书/Answer/session → 一次 startup → ICE nomination → DTLS → SRTP → 媒体 → DELETE、source end、超时或 worker stop`。

WHIP `media/webrtc/whip.cpp:67`、WHEP `media/webrtc/whep.cpp:65` 负责外部创建。基础 SDP、transport/指纹前置检查失败不认领 token；端口资源在授权后准备。完整 codec Answer 失败可能发生在认领后，沿既有一次性语义处理，不建立 token 回滚阶段。

WHIP `media/webrtc/whip_session.cpp:45`、WHEP `media/webrtc/whep_session.cpp:43` 只初始化内部对象、callbacks、接收协程和 timer；后续 startup_media 是 DTLS 导出 SRTP key 后的实际协议阶段，不是重复启动防御。WHIP 成功 startup 后登记 receiver；WHEP 使用独立播放身份及运行期弱 registry。

remote_endpoint_ 表示已 nomination 的远端；dtls_ 存在不代表握手成功；srtp_ 存在才可使用安全媒体。DTLS 重传 deadline、整体建立期限、ICE activity 分别表达独立条件。不能把三个 timer 收为一个通用 transport watchdog。

WHIP `media/webrtc/whip_session.cpp:89`、WHEP `media/webrtc/whep_session.cpp:135` 在 owner 清 registry/订阅、协议对象、timer、UDP 和 port。WHEP 当前已经没有 shutdown_requested_；`:171` 的媒体处理在目标 worker，由 srtp_ 事实阻止关闭后发送。不要照旧文档恢复原子字段。共享 AAC→Opus 输出仍有跨 worker viewer lease 与 finished_，不是 WHEP session 同义状态。

### 3.9 必要的 signaling 控制链

统一授权 `signaling/stream_token.go:101` 在 live→rtspPull→tokens 固定锁顺序下核实当前 source、签发凭据；`:236` 在同一锁顺序下校验 operation、identity、期限与 source running，并删除已认领 token。返回的多个协议 URL 共用这一份一次性授权，不能当作多个可同时重复消费的 viewer 凭据。播放身份、输入 run 的 verifiedAt 和有效期不是同义状态；不增加预授权/回滚层。

GB 点播 `signaling/live_session.go:103` 按 device/channel 复用或建立 live；准备 receiver→INVITE→SSRC 更新→ACK 后 streaming。停止/远端结束进入 dialog 与 receiver 清理，`:404` 的远端关闭结果决定能否释放相关所有权。注册/心跳只更新设备有效性，设备删除 `signaling/device_lifecycle.go:8` 先设置 admission fence 并使旧授权失效，再处理现存 live；不能仅用设备是否还在 map 中替代该 fence。

RTSP 来源启动 `signaling/source_control_http.go:9` 在 sourceOperationMu 下设置持久 desired_state 和 reservation，释放该锁后执行外部 HTTP；`:42` 区分明确拒绝和丢失创建结果，后者经已有补偿仍未确认时保留 unresolved 身份。`:63` 按同一 session identity 确认创建，`:74` 再检查来源是否仍存在/期望仍运行。持久配置与已经创建的远端实例分别保存，PATCH 不等于对运行实例重新初始化，不凭两份 URL/名称就建议合并状态。

`signaling/rtsp_pull_session.go:34` admission 与 WaitGroup 覆盖信令服务终止时仍在执行的控制请求；`:113` 的 stopDone 让并发停止共享一次结果，不让每个请求都发送删除。它是 Go 控制请求协调，不把媒体端异步 DELETE 204 改为等待 C++ 资源释放。

`signaling/media_reconcile.go:14` 先查询媒体事实再按快照 identity 对账，`:149` 再检查对应 RTSP session 仍是同一实例；网络不可达不等于确定 source 已结束。请求失败、设备删除、媒体结束和服务 shutdown 使用各自真实身份/结果，当前没有可无损删除的统一“运行中”状态。

### 3.10 公共执行层的事件边界

source owner 管理固定 tracks、publish/end 和 group 集合；target worker drain 管理该 worker 的 sinks。`media/core/worker_sink_dispatcher.h:193` 将 pending swap 到局部后锁外派发，此时 pending 可空但 drain 尚未结束。remove 与已取得的 snapshot 并发时允许一次迟到 callback；session 的目标 worker 终态事实仍需保留。2500 帧 overflow 终止慢 group，正常 END 排在已有帧后，不变为丢新帧策略。

TCP write/关闭由 `media/net/tcp_transport.cpp:26`、`:50` post 到 socket executor。UDP `media/net/udp_transport.cpp:66`、`:122` 在调用 owner 同步入队/关闭；两者都以 completion capture 保持在途 buffer，不能把 API 外观相似解释为相同线程调用契约。UDP 1 MiB 满时 write 返回 false、丢新 datagram；TCP 高水位报告 no_buffer_space，不强迫上层采用相同策略。

典型关闭时序：T0 RTSP UDP pending read/write；T1 child owner safe_shutdown 清 media 与 handlers；T2 同步关闭 UDP、清 port；T3 迟到 read 错误看到空 handler，write completion 由 transport 的 socket/queue 终态拒绝，已捕获 buffer 保活。GB UDP sender 在 T1 先移空 stream_，WebRTC 先清协议对象并释放 local_port_；这些已经存在的事实替代额外 closed。worker cancellation 可以早于 child safe_shutdown；取消时仍有效的 handler 可请求父关闭，随后沿同一 owner 清理，不恢复 operation_aborted 特殊过滤。

## 四、完整生命周期状态审查

本表覆盖承载阶段、终止、媒体就绪、异步调度及归属的字段；常规 URL、codec 数值、密码、payload 内容、不可变配置不逐项列为状态。资源集合合并列示，源码坐标可追溯。未找到可直接删除的状态字段。

| 协议/层 | 字段及位置 | 表达的事实 | 独立性与分类 |
|---|---|---|---|
| RTSP 连接 | publish_session_/play_session_/transport_，`media/rtsp/rtsp_server_connection.h:68` | 角色对象、TCP 归属 | KEEP；不是 started/closed 副本 |
| RTSP 连接 | input_yield_，`media/rtsp/rtsp_server_connection.h:73` | 当前同步 C 请求可借用的协程上下文 | KEEP；协议 callback 中授权会挂起，不能无限保存普通局部引用 |
| RTSP 发布 | session_id_/descriptions_，`media/rtsp/rtsp_publish_session.h:75` | 协议 Session、ANNOUNCE 轨道描述 | KEEP；验证后续请求，不从对象存在推导 |
| RTSP 发布 | tcp_track_states_/tcp_media_/udp_session_，`media/rtsp/rtsp_publish_session.h:71` | 各轨 SETUP 与传输选择 | KEEP；media 存在不表示所有轨道设置完成 |
| RTSP 发布 | handlers/tcp_rtcp_timer_，`media/rtsp/rtsp_publish_session.h:68` | 父输出/终止/活动通知和 RTCP 任务 | KEEP；clear/move handler 已表达通知可用性 |
| RTSP UDP | 每轨 local_port、RTP/RTCP transport，`media/rtsp/rtsp_publish_udp_session.h:48` | 分配及传输 ownership | KEEP；部分 SETUP 的有效资源可独立存在 |
| RTSP 媒体 | media_stream_/demuxers_/rtcp_sync_，`media/rtsp/rtsp_publish_media.h:65` | 媒体注册、解复用与 sender-report 时钟锚点 | KEEP；同步锚点不是轨道初始配置 |
| RTSP 播放 | playing_，`media/rtsp/rtsp_play_session.h:101` | PLAY 已接受、允许输出 | KEEP；stream_ 在 DESCRIBE 时已存在 |
| RTSP 播放 | waiting_video_track_，`media/rtsp/rtsp_play_session.h:97` | 自然首关键帧门槛 | KEEP；已有一次 optional，不恢复 bool+track 双份状态 |
| RTSP 播放 | track_states_/session_id_/rtcp_sync_，`media/rtsp/rtsp_play_session.h:96` | SETUP 映射、Session 与跨轨时钟 | KEEP；轨道配置不包含已分配 interleaved channel 和 RTCP runtime |
| Pull | config_/resolver_/connect_socket_，`media/rtsp/rtsp_pull_session.h:76` | 已解析业务配置及连接前资源 | KEEP；transport_ 只在 connect 后产生 |
| Pull | client_/media_/transport_，`media/rtsp/rtsp_pull_session.h:81` | C 协议运行、SETUP 后媒体及连接归属 | KEEP；三个释放时机不同，见 V1 |
| Pull 媒体 | initial_video_track_/initial_audio_track_，`media/rtsp/rtsp_pull_media.h:63` | codec config 尚未齐全的固定轨道准备 | KEEP；SDP 可能缺少真实参数集 |
| RTMP 父 | publish_/play_/transport_，`media/rtmp/rtmp_session.h:63` | 所选角色及网络 ownership | KEEP；不增 role/started |
| RTMP 发布 | initial_video_track_/initial_audio_track_，`media/rtmp/rtmp_publish_session.h:53` | 固定拓扑形成前的配置 | KEEP；不是重复 codec 枚举 |
| RTMP 发布 | expected_audio_，`media/rtmp/rtmp_publish_session.h:55` | 元数据未知/明确无音频/期望音频 | KEEP；optional<bool> 三事实无法从当前音频 packet 是否出现推导 |
| RTMP 播放 | initial_packets_，`media/rtmp/rtmp_play_session.h:50` | prepare 暂存初始化输出，startup 后解除暂存 | KEEP；empty vector 与不存在暂存阶段不同 |
| RTMP 播放 | waiting_video_track_/stream_，`media/rtmp/rtmp_play_session.h:48` | 首媒体门槛、有效 generation 与终态 | KEEP；keyframe 尚未到不等于 session 已关闭 |
| RTMP timestamp | started，`media/rtmp/rtmp_timestamp.h:13` | 第一时间戳已建立 | KEEP；时间戳 0 合法，需处理回绕，不是 startup 防重 |
| GB TCP | listener_/transport_，`media/gb28181/gb28181_tcp_receiver_session.h:45` | passive listener 与已建立 TCP | KEEP；accept/connect 的阶段不同 |
| GB TCP sender | stream_/sender_/listener_/transport_，`media/gb28181/gb28181_tcp_sender_session.h:48` | 源 lease、子 packetizer 与连接资源 | KEEP；stream fence 可阻止迟到建立重新启动 |
| GB UDP receiver | local_port_/remote_rtp_endpoint_/remote_rtcp_endpoint_，`media/gb28181/gb28181_udp_receiver_session.h:45` | 本地 reservation 与分别确认的对端 | KEEP；RTCP 地址可与 RTP+1 推导路径不同 |
| GB UDP sender | stream_/local_port_/remote_rtcp_endpoint_，`media/gb28181/gb28181_udp_sender_session.h:47` | 有效源、端口 ownership、可选 RTCP 对端 | KEEP；有源不代表分配成功，port 非远端 |
| GB UDP sender | rtcp_reporting_started_，`media/gb28181/gb28181_udp_sender_session.h:57` | 首次 RTP 后已开始唯一 RTCP reporting 链 | KEEP；timer 等待/完成/写完成之间都可能不处于 pending wait，不能从 timer 推导 |
| GB PS | pending_topology_/announced_topology_，`media/gb28181/gb28181_rtp_receiver.h:75` | 未完成 PSM 与完整公告 | KEEP；未公告不同于公告为空，不能合并 |
| GB PS | unsupported_audio/invalid，`media/gb28181/gb28181_rtp_receiver.h:54` | 不支持音频与非法 topology | KEEP；缺音频本身不等于不支持或非法 |
| GB PS | video/audio codec、track、stream index，`media/gb28181/gb28181_rtp_receiver.h:77` | 编码声明、真实 config 与 demux 通道 | KEEP；三种信息不能互相推导 |
| GB RTP sender | waiting_video_track_/first_media_timestamp_/timestamp_base_，`media/gb28181/gb28181_rtp_sender.h:52` | 门槛与输出时间轴 | KEEP；不是三个终态字段 |
| PS 输出 | source_/muxer_/packet_/waiting_for_key_frame_，`media/ps/mpeg_ps_output.h:61` | 订阅、C mux、当前 packet 与门槛 | KEEP；finish 分别释放，dispatcher 不接管业务 mux |
| HTTP-FLV | source_/writer_/muxer_，`media/http/http_flv_session.h:64` | 源及两个真实 C 输出阶段 | KEEP；FLV mux 与 writer 职责不同 |
| HTTP-FLV | output_queue_/queued_output_bytes_，`media/http/http_flv_session.h:60` | HTTP chunk 串行化与背压 | KEEP；没有另一套 writing_，关闭不能提前清在途 buffer |
| HTTP-FLV | waiting_video_track_/output_buffer_，`media/http/http_flv_session.h:62` | 首媒体门槛与当前同步 writer 累积 | KEEP；累积不是历史缓存 |
| FLV mux | muxer_/tracks_/packet_handler_，`media/flv/flv_muxer.cpp:16`、`:18`、`:29` | C mux 归属、已注册配置、同步输出接口 | KEEP；shutdown 清 handler 后迟到 C 输出返回错误；不再存 closed/error 状态 |
| HLS | source_/tracks_/muxer_，`media/hls/hls_segmenter.h:61`、`:64`、`:71` | 源 generation、TS track 和 mux 归属 | KEEP；旧 viewer 仍需读取结束后的实例 |
| HLS | ended_at_，`media/hls/hls_segmenter.h:68` | 已结束及结束后保留时间起点 | KEEP；muxer 空不能给出保留时间 |
| HLS | waiting_for_key_frame_/has_video_，`media/hls/hls_segmenter.h:69` | 首关键帧门槛与拓扑类型 | KEEP；gate 打开后 has_video 仍为真；内部 audio-only 已有测试 |
| HLS | segment_start_pts_ns_/segments_/next_sequence_，`media/hls/hls_segmenter.h:62`、`:65`、`:66` | 分片边界、窗口与持续编号 | KEEP；不是 per-viewer history/cursor |
| HLS viewer | segmenter_/last_activity_/timer_/subscription，`media/hls/hls_play_session.h:41` | secret lease、HTTP 活动、过期唤醒、worker 停止 | KEEP；源 retention 与 viewer idle 不同 |
| WebRTC | local_port_/UDP transport，`media/webrtc/whep_session.h:74`、`:79`、`media/webrtc/whip_session.h:64`、`:69` | reservation/终态及 I/O | KEEP；port 所有权不能从关闭中的 socket 推导 |
| WebRTC | remote_endpoint_，`media/webrtc/whep_session.h:78`、`media/webrtc/whip_session.h:68` | nomination 完成后的媒体远端 | KEEP；不是最后一个收到 STUN 的地址 |
| WebRTC | dtls_/srtp_，`media/webrtc/whep_session.h:71`、`media/webrtc/whip_session.h:61` | 握手对象与媒体密钥初始化 | KEEP；对象存在不等于 DTLS 连接已完成 |
| WebRTC | 三个 timer，`media/webrtc/whep_session.h:75`、`media/webrtc/whip_session.h:65` | DTLS 重传、建立期限、ICE 活动 | KEEP；不同错误条件与 deadline |
| WHEP | stream_/audio_egress_/packetizer_/waiting_video_track_，`media/webrtc/whep_session.h:68` | 源绑定、共享音频 lease、输出 mux、门槛 | KEEP；不要恢复已删除的 shutdown_requested_ |
| WHIP receiver | media_stream_/audio_track_/SSRC/rtcp_sync_，`media/webrtc/whip_media_receiver.h:53` | 固定拓扑、音频 config、已识别发送者和时钟 | KEEP；SDP payload 类型不替代运行期 SSRC |
| DTLS | ssl_/srtp_keying_material_，`media/webrtc/dtls_transport.h:73` | OpenSSL 活跃对象与导出密钥 | KEEP；late timeout 在 ssl_ 空时拒绝继续 |
| WebRTC packetizer | track_states_/muxer_，`media/webrtc/webrtc_packetizer.h:62` | C mux 的 payload/MID 映射和 ownership | KEEP；有真实 callback 契约 |
| 共享音频 | finished_/viewers_/source_/output_stream_，`media/webrtc/whep_audio_egress.h:43` | 跨 worker 完成 fence、viewer lease 与派生源 | KEEP；结束但仍持有 lease 可达，不能互相替代 |
| Transcoder | timeline_started/next_encoder_pts/next_output_pts_ns，`media/codec/audio_transcoder.cpp:99` | 首输入起点、编码 sample 时间轴、输出 ns 时间轴 | KEEP；PTS 0 合法，单位与推进不同 |
| Transcoder | expected_input_sample_pts/encoded_frame_capacity，`media/codec/audio_transcoder.cpp:101` | 输入不连续判断、可重用 AVFrame 分配容量 | KEEP；不能从输出 PTS/当前 nb_samples 推导 |
| Media core | tracks_/ended_/ps_output_，`media/core/media_stream.h:38`、`:40`、`:41` | 一次固定拓扑、结束且拒绝继续发布、共享派生弱缓存 | KEEP；有轨道的 stream 也能结束 |
| Dispatcher | drain_queued/pending_end/pending/weak sinks，`media/core/worker_sink_dispatcher.h:170` | 唯一 drain、帧后 END、队列、弱订阅 | KEEP；drain 将队列 swap 到局部后 pending 可空而仍在派发 |
| Stream registry | streams_，`media/core/stream_registry.h:29` | 公布的当前 generation 强 ownership | NO_CHANGE；remove 以 identity 避免删除替代者 |
| Session registry | receiver/sender_sessions/stopping_，`media/core/session_registry.h:41` | 控制身份、独立 sender、停止后禁止迟到新增 | KEEP；空 map 不代表进入停止阶段 |
| Worker | tasks_/shutdown callbacks/stop_requested_，`media/net/worker_context.h:116` | 可取消协程、非协程对象清理、跨线程停止请求 | KEEP；不能只 io.stop 丢弃排队释放 |
| TCP transport | write_queue_/queued_write_bytes_/write_callback_/socket，`media/net/tcp_transport.h:46` | 在途串行化、待发字节、通知资格、I/O | KEEP；R1/R2 只删除重复分支，不删除这些资源 |
| UDP transport | pending datagram/endpoint/queue/callback，`media/net/udp_transport.h:46`、`:55` | datagram 边界、逐包目标、在途 buffer、通知资格 | KEEP；callback 被保活以承受同步关闭，无新增 stopped |
| Port pool | allocated ports/mutex，`media/net/media_port_pool.h:29` | 跨 worker reservation | KEEP；端口分配与归还已有唯一实现，不建额外 wrapper |
| Idle timer | last_activity_/weak owner/on_idle_，`media/net/idle_timer.h:33` | 空闲起点、不延长 owner、一次通知 | KEEP；计时不能塞入通用 transport |
| Signaling verify | 局部 resolver completion/deadline，`media/http/signaling_verify.cpp:53` | 迟到 resolve 与整次验证期限 | KEEP；resolver callback 和 HTTP yield 的可达时序不同 |
| 信令 token | tokens/pushRuns/verifiedAt，`signaling/stream_token.go:24`、`:27` | 独立 viewer 凭据、输入 run、已认领输入窗口 | KEEP；有效期、原子消费和当前 source 身份各有意义 |
| 信令 GB live | state/dialog/established/done/cancel，`signaling/live_session.go:29` | SIP 准备、邀请、运行、清理及等待结果 | KEEP；media 存在不表示 SIP ACK/BYE 已完成 |
| 信令设备 | expiry/online、stoppingDevices，`signaling/device_registry.go:20`、`signaling/device_lifecycle.go:8` | 注册期限、已持久化状态及删除期间 admission fence | KEEP；设备配置存在不等于在线 |
| 信令 RTSP | starting/createConfirmed/stopDone，`signaling/rtsp_pull_session.go:22` | HTTP 创建进行中、远端结果确认、共享关闭完成通知 | KEEP；unresolved 与 created 不同，stopDone 不是 C++ 同步删除要求 |
| 信令来源 | desired_state 与 runtime pull、sourceOperationMu，`signaling/source_control_http.go:21`、`signaling/source_store.go:148` | 持久期望、远端事实、控制操作序列 | KEEP；配置 PATCH 与运行实例不是同一份状态，不以一个替代另一个 |

## 五、具体精简候选

### R1 — REMOVE / P2：TCP 高水位局部回调重复判空

- **位置**：`media/net/tcp_transport.cpp:58`、`:67`、`:68`，`safe_write()`。
- **现状**：入口在 socket 未打开或 write_callback_ 空时返回；高水位分支 exchange 将非空 member 回调移至局部，随后又 if(callback)。
- **时序与不变量**：所有生产安装/调用在 socket owner；入口到 exchange 之间仅查询队列与字节数，无 yield、post 执行、用户回调或可调用用户代码的析构。当前没有跨线程修改 callback 的有效调用路径。exchange 得到的局部 callback 必然非空。
- **历史依据**：`2a863a4` 用 socket/callback 入口事实代替 stopped；`a1f2f20` 用 exchange 明确一次性通知。这两项合起来使遗留的局部判空重复。
- **最小建议**：仅去掉局部 if 外壳，直接调用已经移出的 callback。仍先清 member，再报告 no_buffer_space。
- **正常/失败/主动关闭/迟到完成**：普通入队不变；overflow 仍只通知一次；shutdown 后由入口拒绝；在途完成由 on_write 入口拒绝。不要清在途共享 buffer 或修改 overflow 关会话策略。
- **行为影响**：鉴权、输出、回调次数、错误码、关闭顺序和日志均不变。无新增状态/接口/回调，约删 3 行，收益小。
- **测试**：`tests/tcp_transport.cpp:204` 高水位；`:160` 在途关闭；`:135` callback 关闭。CTest `tcp_transport_high_water`、`tcp_transport_in_flight_shutdown`、`tcp_transport_callback_shutdown`（`CMakeLists.txt:367`）。未来实际修改可运行六个 tcp_transport 场景，无必要创建对实现分支重复的测试。
- **证据与风险**：源码不变量充分，未实施；风险低。若将来允许跨 owner 改 callback，必须先重新建立执行器契约，不能把无锁数据竞争视为此 if 能解决的问题。

### R2 — REMOVE / P2：TCP 完成入口之后重复判断 member 回调

- **位置**：`media/net/tcp_transport.cpp:92`、`:97`、`:103`，`on_write()`。
- **现状**：入口已检查 socket、queue 和回调；成功时减字节、pop front，然后再次 if(write_callback_) 才调用。
- **不变量**：入口与该调用之间没有挂起或用户代码。queue 项是 shared_ptr<vector<uint8_t>>，async completion 自身还持有发送 buffer；pop 不会执行修改 callback 的业务析构。有效执行路径上 callback 仍非空。
- **最小建议**：去掉 `:103` 内层 if 外壳直接调用；**保留 `:92` 入口检查和 `:111` 回调之后的检查**。后者面对用户回调重入可改变 queue/callback，是独立保护。
- **具体时序**：T0 async_write 捕获 self/data；T1 成功完成；T2 入口确认资格；T3 记账/pop；T4 callback 可发起 shutdown/overflow/新 write；T5 按 callback 后 queue/callback 事实决定是否发下一包。R2 只改 T3→T4 之间无事件边界的重复判断。
- **删除后路径**：错误仍通知后 return；关闭/overflow 后迟到完成仍被入口 fencing；用户回调触发关闭仍由后置条件与 transport 关闭任务处理，不重新发送已清空队列。
- **行为影响**：回调、网络、资源与日志不变，约删 3 行，不改变 callback 类型或错误传播。
- **测试**：`tests/tcp_transport.cpp:100` 串行写、`:135` callback_shutdown、`:160` in_flight_shutdown，以及既有 socket_error/stalled_reader。当前测试与 owner 契约足以保护该局部改法；本轮未运行。
- **证据与风险**：源码不变量充分，风险低。不能由此扩展为删 callback 前后的全部检查。

### V1 — VERIFY / P2：Pull 成功读取在父关闭后继续推进协议

- **位置**：`media/rtsp/rtsp_pull_session.cpp:147`、`:299`、`:323`、`:329`、`:366`、`:373`；注册点 `media/rtsp/rtsp_pull_media.cpp:222`；transport 写与关闭 `media/net/tcp_transport.cpp:26`、`:50`。
- **分类依据**：这是已有专项的未决边界，**不是新确认生产缺陷，也不是目前可证明删除的字段**。暂不修改、暂不扩展防护的用户决定继续有效。
- **现状**：safe_shutdown 清 media_ 并 post TCP 子关闭；run_read 成功路径直接交 ireader。describe/setup 回调没有 registry identity 检查，setup 可重新创建 media 和安排 RTCP。client 在 run_read 退出之前保持有效，PLAY 已有 media_ 判空保护。
- **源码允许的排列**：T0 DESCRIBE/最后 SETUP 响应成功可读；T1 控制端 take_receiver_session 并 post 父关闭；T2 父 owner 清 media_，post TCP 子关闭；T3 已成功完成的 read 恢复，ireader 调用 describe/setup；T4 同步构造请求经 write post、setup 创建 media/timer；T5 子关闭与新写按实际 executor 队列处理，循环最终遇到关闭错误。T3 是否先于子关闭、同次数据是否含早到 RTP，需要具体运行验证，不能只从取消推断必然 error。
- **已知证据等级**：当前源码能确认 T2 清资源及成功 read 未做终态检查；用户提供的此前专项结论确认过迟到 SETUP+同次早到 RTP 的短暂媒体登记。本轮没有保留在仓库内的新 RED 断言，也没有重跑该专项或取得其原始日志，因此不宣称本轮端到端复现。
- **未证明的后果**：关闭后真实 SETUP/PLAY 发出、持续 source 残留、替代 generation 被删除、对象无法释放、外部 viewer 受损。子关闭通常会拒绝后投递的 write，但不能把一般排队解释当作所有 OS/Asio 排列的证明。
- **现有测试边界**：`tests/rtsp_pull_shutdown.cpp:45` 验证 DNS 迟到与替代身份，`:164` 之后是真实 TCP/生产 session/ireader 的正常及迟到 **PLAY**，`:263` 控制面同样 take+shutdown，`:272` 检查 registry、socket 与对象释放。它没有同样控制 DESCRIBE/SETUP 成功恢复并证明持续业务后果。
- **最小后续步骤**：仅在新报告出现持续残留或实际发送等业务证据时，复用该真实 TCP 测试在相同 owner 排列捕获网络、source identity 和弱引用终态；不恢复预期失败断言、不新增 closed。若届时证明必须阻止推进，再评估在进入协议处理前使用已有 registry identity 的一个局部检查，和在具体回调入口保护相比哪种覆盖更准确。
- **行为与风险**：上述候选 guard 会改变关闭后协议推进和日志，不是行为不变的精简；不能本轮直接实施。现阶段新增状态或统一 guard 框架没有必要。仅凭临时分配不构成修复理由。

## 六、应当保留的设计与覆盖边界

### 6.1 协议阶段与同步输出

- RTSP DESCRIBE/SETUP/PLAY、ANNOUNCE/SETUP/RECORD：对外可达阶段与重复请求检查必须保留。内部一次 startup 已经没有防重 started。
- RTMP prepare/startup：有 Play.Start 前暂存和之后 flush 的严格输出顺序；不是为统一 startup 返回类型即可合并的包装。
- FFmpeg/ireader/OpenSSL callback：同步输入可能生成多个 packet/record，库接口天然要求回调。改成 vector 返回若增加内部累积、拷贝或额外 flush，整体更复杂；当前没有直接删除的收益证据。
- TCP/UDP 完成 callback：推进队列与保存 in-flight buffer 是真实异步职责；不得以“可以使用协程”为由在同步媒体输出处再 spawn。
- HLS playlist 等待已用 yield 顺序执行；RTCP/DTLS timer 是单次异步事件或周期协议任务，不为消灭 callback 新建协程。

启动返回值也已按真实职责区分，而非统一 bool：RTMP/RTSP 父连接、Pull 与 GB TCP active sender 只启动任务，使用 void；GB TCP passive sender 在 bind/listen 时同步失败（`media/gb28181/gb28181_tcp_sender_session.cpp:29`），需 bool。UDP receiver 返回已分配端口；WHIP/WHEP 报告 DTLS/媒体内部初始化失败；PS/HLS/mux/transcoder 可能无法建立第三方资源，不能删失败传播。RTMP play startup 在 `media/rtmp/rtmp_play_session.cpp:51` 还可能发送初始配置失败，也不能只因它不做授权就改为 void。

### 6.2 source/generation、轨道和关键帧

`media/core/media_stream.cpp:21` 一次固定 tracks；`:69` 对 ended、空 payload、未知轨道拒绝发布；`:83` end 同时终止 dispatcher。ended_ 不能从 tracks 或 sink 是否为空推导。`media/core/stream_registry.cpp:12` 的对象身份与 `media/core/session_registry.cpp:20` 之后的接收身份不能简化为仅按字符串删除。

共享 PS 及 WHEP 音频以 source generation 绑定，派生弱缓存和 viewer lease 分工明确。自然首关键帧门槛是输出解码正确性，不能恢复 source keyframe request、history/cursor、GOP replay 或 on_discontinuity。RTMP metadata、RTSP SDP 与 PS PSM 不保证 codec config 已齐全，initial track 与 runtime codec/stream index 要保留。

### 6.3 worker stop、终态和取消

`service.cpp:85` 先关闭 WebRTC runtime 和 session registry，再 request_stop；`media/core/session_registry.cpp:142` detach registry 后在锁外关闭；`media/net/worker_context.cpp:66` 请求只执行一次，在 owner 先发协程取消、再调用 non-coroutine shutdown subscriptions、最后释放 work。不是等待全局全部 session 清理的 barrier，也不是 listener 持有所有连接。

取消请求不保证成功完成的 async_connect/read 自动改报 operation_aborted。GB TCP `media/gb28181/gb28181_tcp_sender_session.cpp:64`、receiver `media/gb28181/gb28181_tcp_receiver_session.cpp:93`、Pull resolve/connect 的 cancellation 要保留。`service.cpp:56` 的 operation_aborted 退出 listener，与普通 accept 错误固定 3 秒重试语义不同。不能统一成普通网络错误后重试关闭中的 listener。

上一轮纯日志过滤已经删除；RTSP UDP handler、GB UDP stream fence 代替没有独立语义的取消码过滤。本轮没有发现需要恢复这些取消过滤的依据，不新增关闭状态。日志内容变化已属于既有提交，不混入 R1/R2 的无日志变化结论。

### 6.4 targeted 测试能证明什么

下列均为**读取已有测试的覆盖分析**，不是本轮执行结果。真实协议历史报告只作背景，不替代当前时序测试。

| 模块 | 现有测试与注册 | 已保护的关键路径 | 仍不能据此宣称 |
|---|---|---|---|
| RTSP 发布 | `tests/rtsp_publish.cpp:291`、`:301`；CTest rtsp_publish（`CMakeLists.txt:403`） | TEARDOWN、UDP pending read、父关闭/worker stop、通知和端口释放 | 所有部分 SETUP 失败点都已逐点注入 |
| RTSP/RTMP 父启动 | `tests/session_start_stop.cpp`；`CMakeLists.txt:378` | before_start、queued_start、queued_shutdown、active_read | 全部网络包与删除交错排列 |
| RTSP/RTMP 播放授权 | `tests/protocol_play_auth.cpp:343`、`:387`、`:500`；`CMakeLists.txt:420` | 授权先于 mux、RTMP Play.Start 顺序、迟到输出、RTSP generation 替换 | RTMP 在每一个授权挂起点源替换都已单独覆盖 |
| RTMP 真实媒体 | `tests/rtmp_decode.py:41`；`CMakeLists.txt:448` | FFmpeg 实际发布，再拉流解码；并非只有输出测试 | 错误 metadata/任意 codec config 时序都已穷举 |
| Pull | `tests/rtsp_pull_shutdown.cpp:45`、`:164`；`CMakeLists.txt:387` | DNS 迟到身份、真实 TCP 正常/late PLAY、socket/weak lifetime | V1 late DESCRIBE/SETUP 持续后果 |
| GB sender | `tests/gb_sender_lifecycle.cpp:102`、`:163`、`:214` | 迟到输出、排队帧、parent/child 顺序、worker stop | 所有第三方 C 回调错误都注入过 |
| GB TCP | `tests/gb_tcp_lifecycle.cpp:36`、`:94`、`:118`；`CMakeLists.txt:349` | queued read、pending accept、connected/refused、关闭资源 | 任意 active/passive 建立和 SIP 控制交错 |
| GB PS | `tests/gb28181_ps_receiver.cpp`；`CMakeLists.txt:355` | topology/config/媒体注册的分支契约 | 一切摄像机私有异常 PS 都可正确解释 |
| UDP | `tests/udp_transport.cpp:69`、`:128`、`:177`、`:231`、`:330` | 序列/endpoint、在途 shutdown、overflow/reentrant、socket error、GB 策略 | 其他协议所有 packet/timer 排列 |
| TCP | `tests/tcp_transport.cpp:100`、`:135`、`:160`、`:204`；`CMakeLists.txt:367` | 串行、callback 关闭、在途 buffer、高水位/慢读/socket 错误 | 可脱离 owner 多线程调用 member |
| FLV | `tests/http_flv_auth.cpp:125`、`:360`；`CMakeLists.txt:411` | 拒绝不分配 writer/mux、授权与生命周期 | 4 MiB overflow 与每个 pending chunk 完成排列都有直接测试 |
| HLS | `tests/hls_segmenter.cpp:73`、`:101`、`:170`；`tests/stream_auth.py` | audio-only 内部契约、拒绝无资源、playlist ready/timeout/cancel、一次性身份与后续访问 | 所有旧 viewer 持 buffer 同时跨保留窗口的排列 |
| WebRTC | `tests/webrtc_auth.cpp`、`tests/whep_lifecycle.cpp`；`CMakeLists.txt:428` | 资源前授权、源替换、排队帧/派生 lease 释放 | 每项单元测试都实际完成 ICE/DTLS/SRTP |
| DTLS | `tests/whep_dtls_timeout.cpp`；`CMakeLists.txt:437` | timeout、late completion、establishment；SSL 释放后不重启 | 当前 Goal 已重新跑 Chrome 真媒体 |
| 媒体/worker | `tests/media_lifecycle.cpp:214`、`:293`、`:351`、`:385`、`:480` | generation、派生 lifetime、registry identity/stop、父子清理 | 不需要任何 shutdown subscription |
| Dispatcher | `tests/worker_sink_fanout.cpp:152`、`:184`、`:223`、`:259`、`:306` | inline、worker grouping、弱 ownership、remove/snapshot、END 与 2500 overflow | remove 返回后不可能还有已 snapshot 的一次 callback |
| 信令 | `signaling/stream_token_test.go`、`signaling/play_http_test.go`、`signaling/live_session_test.go`、`signaling/device_lifecycle_test.go`、`signaling/media_reconcile_test.go` | 原子认领、live 共享/停止、设备删除、远端对账 | 无法注入的每个数据库/HTTP 丢响应组合都有覆盖 |

上述覆盖边界不是新增 VERIFY 精简项。只有需要删除具体保护、或出现实际后果时才补定向测试；不为表格的空缺立即新建一批测试任务。

## 七、跨协议重复职责与抽象边界

| 看似重复的实现 | 现有事实来源/差异 | 结论 |
|---|---|---|
| 网络 async write/queue | TCP/UDP 各已有唯一 transport；UDP 逐 datagram/endpoint/drop，TCP 字节流/overflow 通知 | NO_CHANGE；职责一致，不强求源码一致 |
| media/PS sink fanout | 已统一 `media/core/worker_sink_dispatcher.h:27`；同 worker inline、跨 worker 分组/2500 上限/END | NO_CHANGE；无第二套 drain 算法，不新建 pipeline |
| RTSP UDP/GB UDP RTCP timer | 启动点、RTP统计、对端确认和报告策略不同 | KEEP；不抽配置/策略型周期任务类 |
| WHIP/WHEP UDP/STUN/DTLS | 外部协商相似，发布注册与播放订阅/共享音频/关键帧不同 | KEEP；没有推翻既有 WebRTC transport 不提取结论的新证据 |
| 多个 safe_shutdown/post | 分别切换到 owner、解除独立订阅、销毁各自 C 资源、断开父回调保活 | KEEP；逐层所有权，不是共享关闭状态机 |
| HTTP-FLV queue 与 TCP queue | HTTP chunk framing、await 中 buffer lifetime、4 MiB 背压；不是裸 TCP write ownership | KEEP；合并会泄漏 HTTP 语义 |
| HLS segment deque | 多请求共享 retention，sequence/playlist、结束后窗口 | KEEP；不是 write backlog，不套通用网络 queue |
| 注册表 map/mutex/identity | stream 当前 generation、receiver/sender 控制身份、token claim、SIP dialog 是不同事实 | KEEP；不能用一个 generic registry 与 callback policy 代替 |
| 各 codec mapping/track 初始化 | SDP/FLV/PS 来源不同；已有 codec/sdp helpers 和 mux/demux库 | NO_CHANGE；没有完整 ownership 算法的重复证据，小 switch 相似不足以建层 |
| listener accept 与 TCP active connect | 真实连接角色、同步 bind 失败与异步建立不同 | KEEP；已有 transport/listener，通用 connection wrapper 无明确收益 |

当前抽象边界合理。transport 不知道协议；dispatcher 不知道 codec；session 保留协议 timer 和结束决定；registry 不等待关闭完成。未发现调用方为适配这三类抽象新建额外重复终态或配置层。同步 C callback、跨 worker source lease 和控制身份不能仅按数量视为抽象过度。

## 八、建议实施顺序（仅计划）

| 组 | 涉及文件与拟调整 | 前置验证与回归 | 收益与依赖 |
|---|---|---|---|
| G1 | `media/net/tcp_transport.cpp`；R1/R2 只去两个 if 外壳 | 用户 Review 后授权；重核 owner 回调调用点；执行六个 tcp_transport 场景、普通构建和适当 CTest | 同一根因可一次提交；约删 6 行，无字段/接口/日志变化，无跨协议改动 |
| G2 | V1 只保留现有调查记录；有持续业务损害新证据才恢复真实 TCP 定向调查 | 必须区分成功 read、父关闭、子关闭、新 write 与早 RTP；正常对照、source identity、peer 实际字节与弱引用终态 | 不依赖 G1；现阶段不建议代码修改，更不新增通用 fence |
| G3 | 其他协议无拟实施改动 | 正常后续开发时沿用已有 lifecycle/授权/跨协议测试；没有需求不追加全量实验 | 收益是避免回流历史状态/抽象，不制造“精简”提交 |

普通 startup void 已用于没有同步失败的父连接/拉流任务；有真实 port、mux、DTLS 初始化失败的 bool/optional 保留。不存在需要先统一所有签名才能实施 G1 的依赖。

## 九、需要用户确认的取舍

本轮没有必须新增的重大架构选择。G1 等待报告 Review 及后续实施授权；其余保持当前模型。

五项既定行为继续接受，不作为整改清单：停止推流旧输入迟到登记只记录；GB UDP 收到数据刷新 idle；SSRC 更新异步 204；接收器删除异步 204；INFO 保留完整一次性 token/URL。V1 的暂不修复决定不重新开放，除非新增实际业务后果证据。

数据库期望配置与运行实例、SIP 终止与 C++ 异步删除、HLS retained segment 与 viewer idle 都有真实边界。没有理由为本轮精简新增同步 HTTP 控制、完成等待、future/promise、延迟关闭或恢复策略。

## 十、最终审查结论

- **可以直接实施**：R1、R2，两处重复 TCP callback 判空；仅局部小收益。
- **必须先有新证据才验证/修改**：V1，迟到成功 DESCRIBE/SETUP 的持续业务后果。已知短暂登记不能自动判定为缺陷。
- **应当保留**：第四、六部分协议阶段、轨道形成、自然关键帧、generation、异步取消与共享资源归属；不恢复已删除的状态。
- **当前不值得修改**：RTCP timer 框架、WHIP/WHEP common transport、通用连接/registry wrapper、小 helper/重复 switch 和为测试覆盖表新增防御代码。

全部指定协议和公共生命周期范围均完成源码/历史/现有测试审查。未做运行验证的部分明确保留边界；未逐行审查依赖源码，也不把缺少运行证据写成 PASS。没有生产代码、测试、性能工具、构建、signaling 或 third 改动。唯一交付物是本报告；不包含补丁或自动实施动作。
