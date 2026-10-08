# WHEP / WHIP transport 只读设计审计

审计基线：`90cdd7fd2d26de0a74540d288c7d418df3f9a16c`。fanout 实现、普通/San/真实协议/WHEP 容量验证及验证文档均已 commit/push 后才开始本审计。本次只增加报告，未修改任何 WebRTC 源码，也未实现共同 transport。

结论：**当前不做完整的共同 webrtc_transport**。剩余协调代码确实重复，但预计生产净减少约 80–180 行，同时需要三个新的组件边界回调、两阶段启动和额外的跨 worker 停止协调；现有 UDP、STUN、DTLS、SRTP 的实质算法已经分别只有一份。它与本轮可机械搬移、零新增运行时回调的 fanout 提取不同，当前无法仅凭静态审计证明整体复杂度下降。

## 现有唯一 helper

| 模块 | 已承担的职责 | 证据 |
| --- | --- | --- |
| udp_transport | socket、datagram queue、endpoint/buffer ownership、串行发送、overflow 拒绝与网络错误上报 | media/net/udp_transport.h:23；udp_transport.cpp:75、104 |
| media_port_pool | reservation、绑定重试、失败释放及端口释放 | media/net/media_port_pool.cpp:99 |
| stun_message | Binding 解析、username/MESSAGE-INTEGRITY/fingerprint 校验、属性解析、响应编码 | media/webrtc/stun_message.cpp:361、421 |
| dtls_transport | OpenSSL 握手、fingerprint 校验、timeout 处理、SRTP keying material 导出、出站 record callback | media/webrtc/dtls_transport.h:21；dtls_transport.cpp:200、321 |
| srtp_transport | inbound/outbound libSRTP context、RTP/RTCP protect/unprotect | media/webrtc/srtp_transport.h:15；srtp_transport.cpp:91 |
| webrtc_sdp | offer 解析、两种 answer、codec/payload/mid 协商 | media/webrtc/webrtc_sdp.h:26 |

待提取部分不是重新实现这些算法，而是 UDP/ICE/DTLS/timer 与媒体就绪之间的协调。组件不能知道 media_stream、packetizer、receiver、source generation 或 session registry。

## 重复状态：计数口径

头文件证据为 `whep_session.h:76` 和 `whip_session.h:65` 开始的成员区。

| 可共同持有的网络协调字段 | 数量 |
| --- | ---: |
| dtls_、udp_transport_ | 2 |
| dtls_timer_、establishment_timer_、ice_activity_timer_ | 3 |
| remote_endpoint_ | 1 |
| local_port_reservation_ | 1 |
| ice_ufrag_、ice_pwd_、remote_ice_ufrag_ | 3 |
| **合计** | **10** |

两类实际共有 **14 个字段**：上述 10 个加 worker_、srtp_、id_、answer_。后四个不能混入“可以从 session 消失的网络字段”计数：worker 还服务媒体对象；SRTP 的使用由媒体方向决定；id/answer 包含 session 和 SDP 协商信息。

若计入 SRTP，则共同网络/安全字段是 **11**，但本审计的最窄候选将 SRTP 留在 session，因此采用 **10**。没有把三个 ICE 字段或三个 timer 合并算成一个。

WHEP 必须保留 stream_、audio_egress_、waiting_video_track_、packetizer_ 和 shutdown_requested_；WHIP 必须保留 stream_name_、media_receiver_。两边继续持有各自 srtp_、answer_、id_、worker_。source identity/generation 不转移：WHEP 持有具体 stream 对象并按 sink 身份移除，WHIP receiver 负责 source 的登记和生命周期。

## 重复方法

按重载分别计数，网络协调方法 **10 对**，WHEP 261 行、WHIP 229 行，共 **490 个物理行**，含签名、空白和日志。它们并非全部逐字相同：

| 方法对 | WHEP 行 | WHIP 行 | 差异 |
| --- | --- | --- | --- |
| run_udp | 248–267 | 181–200 | 同构 read loop / error / shutdown |
| handle_packet | 269–306 | 202–230 | WHEP 忽略入站 SRTP；WHIP 交给 handle_srtp；guard 和日志不同 |
| handle_stun | 308–394 | 232–297 | 认证、420/487、nomination/activity 同构；日志不同 |
| handle_dtls | 396–419 | 299–322 | DTLS transition 同构，调用不同媒体初始化 |
| send_udp(packet) | 489–496 | 374–381 | 无 nomination endpoint 时不发送 |
| send_udp(packet, endpoint) | 498–508 | 383–393 | atomic fencing 与 port sentinel 不同 |
| schedule_dtls_timeout | 510–534 | 395–418 | guard 不同，其余同构 |
| handle_dtls_timeout | 536–551 | 420–434 | guard 不同，其余同构 |
| startup_establishment_timeout | 553–568 | 436–451 | 完成条件分别为 packetizer_ / media_receiver_ |
| refresh_ice_activity_timeout | 570–585 | 453–468 | guard 不同，其余同构 |

另有 **2 对混合关闭方法**：shutdown（WHEP:188 / WHIP:148）和 safe_shutdown（WHEP:199 / WHIP:154），共 59 行；加入后是 **12 对、549 行**，不能把其中媒体/source 清理整体搬进 transport。

startup 和 startup_media 还有局部共同步骤，但不是可以整体迁移的同构方法；未把它们算进上述 12 对。startup_media 的 WHEP:421 和 WHIP:347 都初始化 SRTP，主体分别创建 packetizer 与 media_receiver，须保留。id accessor 和 random_hex 不计入网络协调方法数。

当前 ICE-Lite SDP 宣告在 webrtc_sdp 中，session 的 ICE controlled 行为表现为拒绝对端 ICE-CONTROLLED、要求 ICE-CONTROLLING。自 `cc63c0d` 起，只有通过当前会话 USERNAME、MESSAGE-INTEGRITY 和 FINGERPRINT 验证、具备 PRIORITY 且带 USE-CANDIDATE 的请求才能建立或更新 nominated endpoint；未知必需属性和错误角色不更新 endpoint，普通连通性检查也不更新。DTLS/媒体仍只接受当前 nominated endpoint，此更新沿用同一会话凭据，不实现 ICE restart。STUN 响应仍可能发往未 nomination 的 endpoint，不能将 UDP 所有写入固定成唯一 destination。本文的方法行数及提取成本仍按开头的历史审计基线统计。

## 如果提取，最小候选接口

以下仅为假设接口，不是已实现或已验证的声明：

```cpp
webrtc_transport(worker_context& owner, callbacks);
expected<local_ice_parameters, error> bind(ip::address address);
bool start(std::string remote_ufrag, std::string remote_fingerprint,
           const dtls_certificate& certificate);
bool write(std::vector<std::uint8_t> packet);
void request_stop();  // 同步跨 worker fencing，不在 caller worker 清资源
void shutdown();      // owner worker 清理 I/O、timer、callback 和 reservation
```

local_ice_parameters 只包含本地 port/ufrag/password。bind 后 session 用这些值生成原 SDP answer、确定 transport_mid，再把选中的远端属性交给 start。不能用“先传整个 offer 给 transport”隐藏 SDP 选择，也不能在不知道 local port/credentials 时先生成 answer。失败分类应映射回现有 session startup error，不引入通用配置层。

公共 write 只需向已 nomination peer 发送媒体 datagram；STUN 的逐包 endpoint 发送留在组件内部，继续调用 udp_transport 的 endpoint API。返回 false 表示入队拒绝，保留 drop-newest/继续 session；真实网络错误经 failure 回调报告，由 session 决定关闭。没有恢复兼容 wrapper、retry 或 transport 直接 shutdown session 的接口。

最少 **3 类固定回调**，不加 timeout、nomination、metrics、overflow 等额外回调：

| 回调 | 候选契约 |
| --- | --- |
| bool on_dtls_ready(const dtls_srtp_keying_material&) | 每连接首次 DTLS 成功时，session 初始化自己的 SRTP 和 packetizer/receiver；返回成功后才取消 establishment timer。初始化失败由 session 清理部分媒体状态，不能把 DTLS connected 等同于媒体 established。 |
| void on_media_datagram(span<const uint8_t>) | owner worker 内 inline 调用；span 仅本次回调有效，保持当前 read buffer lifetime，不能新增 post/媒体字节复制。WHIP 解保护并交 receiver；WHEP 保持忽略入站 SRTP，不增加反馈处理。 |
| void on_failure(transport_error) | UDP read/send、DTLS 或 timeout 的失败通知；当前 operation 停止，但 session 仍决定生命周期。必须去重并处理回调中 shutdown 的重入。 |

DTLS timeout、ICE activity timeout 不需要各一个回调；组件可内部负责 timer。establishment timeout 必须等待上述媒体就绪确认，不能在 DTLS 一完成就提前取消。

这三个是**新组件与 session 之间**的回调；已有 DTLS send_handler 和 UDP write_callback 仍存在于内部，不将它们隐去或误称为零运行时回调。WHIP 每个入站媒体 datagram 会多经过一层回调，性能收益不能默认成立。

## 停止时序是最难的边界

WHEP shutdown 在调用线程先 `shutdown_requested_.exchange(true)`、remove source sink，再 dispatch safe_shutdown（whep_session.cpp:188）。on_frame 在 packetizer 调用前检查该 atomic（:222）。remove 不撤销旧 fanout snapshot，本轮提取正是保留了这个窗口。

因此共同组件不能只检查自己 owner worker 上的 reservation 或 udp_transport::stopped_。从 WHEP 原子关闭请求到 owner safe_shutdown 之间，旧版 handle_packet/timer 已经因 session atomic 停止；新组件也必须同步禁止 ICE/DTLS/媒体事件，故候选 API 列出 request_stop。它不能删除 WHEP 的媒体/source fencing，也不能越过 session 在 caller worker 释放资源。

这会增加新的 transport 停止协调，或需要第四个“是否仍允许事件”的回调。后者又把 session 终态查询带回组件、增加逐包间接调用，因此不算更简单的三回调方案。候选三回调方案仍需证明 callback 的弱 ownership、read handler 存活、回调中关闭、DTLS/媒体/端口清理先后与现有语义一致，不能把 safe_shutdown 一次整体下沉。

## 净收益判断

490 行网络方法不是 490 行可直接净删除：新组件仍要保留其中一份 STUN/DTLS/read/timer 算法、原日志及初始化；关闭方法仍保留业务部分，两个 session 还要绑定回调和处理 startup error。

按现有物理行、包含新头/实现、两个 session 适配和停止协调，生产净减少估计 **80–180 行**，不含新增测试/文档。若为了保留差异将 timer 或 endpoint 再留回 session，净收益可能仅 20–80 行。没有实现 prototype，因此这是静态估计，不是已测量的代码差异或性能结论。

它可把 10 个网络字段和 10 对方法从 session 移走，减少 ICE/timer 修改的双点维护；但 source identity、媒体方向、SRTP 使用、session registration、WHEP 跨 worker fencing 均不会消失。新增绑定、启动阶段和关闭协作也不会因少了字段就自动变简单。

最终建议：**不做当前这次完整 webrtc_transport 提取**。现有 helper 的边界已清晰；与净删除量相比，新生命周期接口和逐包回调的成本尚无现实需求或实测收益支持。本轮不扩大架构、不提出新的实现任务，保留只读审计结论。
