# HLS 授权修复与历史复杂度复核

日期：2026-10-10

BASE_HEAD：`5b8f992dd8e66329a330fac4f270d2675397de03`

分支：`fix/hls-auth-complexity-audit`

HLS 修复提交：`f1a6d49cd535d66b52894dfd44090dc6a4459bfe`

结论：修复了一处授权前创建媒体处理资源的问题。历史审计没有找到可以证明安全删除的新增状态、接口或关闭协调；不实施额外精简。进程停止机制确实在历史硬退出方案之后恢复，但属于明确提交的生命周期取舍，并未恢复关闭完成等待。生产改动只有 HLS 请求中六行代码的位置调整，净 LOC 为 0，新增状态、接口和同步机制均为 0。

## HLS 修复与时序

`media/http/hls_http_session.cpp:90` 原来先固定 source，再调用 `hls::get_or_create()`，最后请求授权。分片器可能已创建 muxer、订阅并持有 source，随后才返回 403。

现在执行：固定 source → `verify_stream()` → 确认 HTTP socket 仍打开 → `hls::get_or_create()` → `hls_play_session::create()` → 307。无效、过期、已消费令牌的共同拒绝路径不再执行两个资源创建入口。媒体服务器继续委托信令原子认领令牌，不存储或恢复令牌。

授权等待期间仍持有原 source，不重新按 ID 查找另一个对象。原 source 若已结束，`media_stream::add_sink_owner()` 会向目标 worker 投递 `on_end()`；不会在分片器持锁初始化时同步重入 `finish()`。本轮没有为这一窗口增加预授权状态、重新认领或回滚。授权认领后初始化失败仍可能消耗令牌，符合一次性授权约定。该窗口完成了代码时序分析，没有声称新增故障注入覆盖。

原有分片窗口、2 秒切片目标、结束后保留、播放身份 30 秒空闲期限均保持。不同播放身份继续引用同一 source generation 的分片器。

### RED / GREEN

- 在原生产代码上新增真实 HLS HTTP handler 回归：授权替身返回 403，随后移除 registry、结束并释放 source，检查公共 `weak_ptr` 生命周期。实际 RED 为 `denied HLS request retained the source through a segmenter or viewer`；不是人为修改生产代码制造失败。
- 移动创建步骤后，同一测试 GREEN。403 不包含播放会话 Location，source 不再被未经授权创建的资源持有；无生产专用查询 API。
- 扩展 `tests/stream_auth.py`：无效、过期令牌拒绝；合法单次认领及重放拒绝；后续 playlist/segment；两份独立观看身份读取相同既有分片并比较内容；FFmpeg 实际解码；停止源后两个身份取得 ENDLIST 和保留分片。过期判断只添加在已有外部授权测试替身中，未修改生产令牌模型。

## 历史与当前事实

阅读最近 55 个提交、相关早期精简提交，以及媒体核心、基础设施重复审计、WebRTC 审计、统一授权计划和系统验证文档。三个单轮只读调查分别检查 worker 停止、协议会话状态和控制/计时器；主代理核对关键源码及历史 diff。源码证据以本分支 HLS 修复提交为准。

| 历史取舍 | 当前复核 |
| --- | --- |
| `6e9580f`、`2690f6d` 使用硬退出绕过待处理 stackful 协程析构 | 后续 `26c846e` 跟踪并取消协程，`8e52c0a` 自然结束 worker，`f3f2bf2` 关闭普通异步会话，`5f0d1e5` 用 worker 停止处理信号。这是明确恢复，不能只撤掉取消或清理、再让活跃协程随 io_context 析构。 |
| `0666490` 将受控会话与 WebRTC 纳入服务停止，`02688cb` 删除未用强制停止接口 | `worker_context` 自 `02688cb` 至审查基线无代码变化。没有新任务计数、全局完成 barrier 或 listener 连接表。 |
| `ecb03b7`、`c328048`、`f509388` 删除一次性启动守卫与同步媒体辅助终态；`836d9b9` 删除 WebRTC started/config 状态 | 这些 started/closed/initialized 防重复字段没有恢复。保留资源事实和真实跨线程 fence。 |
| `1219a98`、`874b4f5` 曾增加接收关闭等待 | `3e99476` 已撤销 closing 槽位、完成结果、等待者及 HTTP 协程等待；当前没有重新引入。 |
| `6d10f0e`、`8e9432f`、`b898f2a`、`02d6a50`、`87bc65c` 将启动失败清理交创建方 | 当前每个创建方处理返回失败并调用已有 shutdown；没有新失败状态机。`startup()` 的 bool/expected 报告实际同步初始化错误。 |
| `17c1904` 统一会话 shutdown 投递到所属 io | session 自己清理组合成员，socket 关闭后归还端口；没有关闭完成通知或延迟关闭 timer。 |
| `198f61d` 固定输入空闲时长并删除重复 owner 检查 | 当前 idle_timer 仍是固定 20 秒、weak owner、last_activity 和一个回调；没有每包 timer 或额外 watchdog。 |
| `4bd3b66` 使用运行 ID，`0a8f3f9` / `2bde22f` 统一单次授权 | 原名称加运行 ID 的复合登记已简化；对象身份 remove、独立播放身份和统一 verify 对应当前正式需求。 |

## 候选逐项判定

### 1. service 停止、registry 与 worker 取消

证据：`service.cpp:85`、`media/core/session_registry.cpp:142`、`media/net/worker_context.cpp:66`、`media/net/worker_context.h:91`。

1. 真实问题：停止 listener/read 协程，摘除强登记会话，终止普通 callback/timer 链，让 owner executor 运行取消完成和资源清理。
2. 重复性：registry 管登记与停止后的 add；worker 管所有协程及不在 registry 的订阅者，覆盖对象不同。
3. 历史原因：早期硬停最短，但须跳过待处理协程析构；上述后续提交明确采用可取消、自然退出方案。
4. 后续需求：HLS 普通 HTTP callback/身份 timer、GB UDP sender 和共享音频输出需要 worker 停止通知，协程取消不能代替这些普通回调的终止。
5. 删除正确性：不能证明。仅释放 work guard 会留下 accept/read/timer 工作；仅 io.stop 不执行既有清理链。registry 的 `stopping_` 还拒绝停止期间的新登记。
6. 简化收益：表面可少一套入口，却不能消除其独立事实；替代方案涉及新的进程退出与 ownership 决策。
7. 风险/结论：活跃异步操作、强登记和协程析构风险超过收益，保留。没有全局完成计数、等待循环或 future；worker join 是线程终止，不是新增会话完成协议。

### 2. WHIP 重叠关闭与各会话 shutdown/post

证据：`media/webrtc/whip.cpp:93`、`:144`、`media/webrtc/whip_session.cpp:163`、`media/rtmp/rtmp_session.cpp:273`、`media/rtsp/rtsp_server_connection.cpp:420`。

1. 真实问题：WHIP 使用 stream ID 的 receiver 强登记及 WHEP/WHIP HTTP resource ID 的弱索引；各会话在 owner worker 释放协议资源。
2. 重复性：服务停止时 WHIP runtime 和 receiver registry 确实可对同一对象重复投递 shutdown，读取消也可能再次请求关闭。
3. 历史原因：`0666490` 保证受控/WebRTC 会话进入服务停止路径；`17c1904` 明确投递清理，避免回调栈内立即拆除资源。
4. 后续需求：统一授权要求 prepare/verify/activate 时序和未激活失败清理，不能仅依赖活跃 UDP read 退出。
5. 删除正确性：尚不能证明所有准备、登记和 runtime 清理路径由一个入口完全覆盖。当前 reset/cancel/identity remove 可重复，WHIP 端口用已有 `local_port_ != 0` 保证只释放一次。
6. 简化收益：删少量调用不能消除索引或 owner 边界；添加“关闭中”标志只会增加状态。
7. 风险/结论：保留，重复投递本身未形成已确认 correctness 问题。WHIP 的媒体输入及 timer 在自身 worker 执行，不因 WHEP 有 atomic 就给 WHIP 复制一个。父 session、子 session 和 transport 的不同清理职责不能合并为互相关闭框架。

### 3. 生命周期标志与可推导状态

证据：`media/webrtc/whep_session.cpp:211`、`media/webrtc/whep_audio_egress.cpp:100`、`media/rtsp/rtsp_play_session.cpp:67`、`media/gb28181/gb28181_udp_sender_session.cpp:180`。

1. 真实问题：WHEP/GB RTP sender 的跨线程 remove/snapshot fence、共享音频 finish/acquire/release、RTSP PLAY 阶段和 GB 首次 RTP 后只启动一次 RTCP 链。
2. 重复性：这些事实与 socket 打开、stream 指针或 timer 已存在不等价。RTSP 在 DESCRIBE/SETUP 时已经有资源，但尚未 PLAY。
3. 历史原因：`042c984` 删除 RTSP closed 并保留 playing；`836d9b9` 删除 WebRTC started 并保留 shutdown_requested；`e9d4422` 明确 RTCP reporting 的独立事实。
4. 后续需求：仍存在迟到 fanout snapshot、未 PLAY、首包建立报告链等可达时序；没有因新抽象而恢复旧 started 防重入。
5. 删除正确性：否。会放行关闭后的帧、混淆协议阶段或多次启动 RTCP 重排链。
6. 简化收益：不能无代价删除这些状态；资源指针已经用于普通幂等清理，未增加第二份 closed/ready 真相。
7. 风险/结论：保留必要 fence/协议事实。没有确认可删标志；HLS/PS 的自然关键帧门、media_stream ended 和 transport stopped 同样有既有独立职责。

### 4. 启动失败返回值与统一清理

证据：`media/webrtc/whip.cpp:79`、`media/webrtc/whip_session.cpp:61`、`service.cpp:39`；历史五个“启动失败交由上层清理”提交。

1. 真实问题：bind、SDP/codec/muxer 初始化等同步失败必须返回创建方，清理由已取得资源的 session 统一执行。
2. 重复性：同步返回错误与稍后 owner 清理不是两份错误状态；未缓存额外 error/ready/initialized 标志。
3. 历史原因：删除的是只调用一次 startup 的防重复守卫和局部重复回滚，不是实际失败传播。
4. 后续需求：统一授权前做协议预检，失败必须保持已有端口、对象及 callback 清理顺序。
5. 删除正确性：否。忽略返回值或删创建方 shutdown 可保留端口或初始化中的资源；把清理再塞回多处失败分支会恢复重复逻辑。
6. 简化收益：当前已比多处分支回滚更直接，未发现可删控制阶段。
7. 风险/结论：保留；`media_port_pool` 失败归还测试、协议/auth 集成保护当前边界。

### 5. idle、DNS 与 HLS timer

证据：`media/net/idle_timer.cpp:26`、`media/http/signaling_verify.cpp:59`、`:75`、`media/hls/hls.cpp:51`、`media/hls/hls_play_session.cpp:84`、`media/http/hls_http_session.cpp:158`。

1. 真实问题：输入空闲、授权总 deadline、首个 playlist 等待、播放身份 inactivity 和 ended 分片器 retention。
2. 重复性：DNS timer 覆盖 resolver，tcp_stream deadline 覆盖后续连接/读写，两者共用同一绝对 3 秒上限；三个 HLS timer 分别管理请求、身份和缓存。
3. 历史原因：`198f61d` 删固定 timeout 配置与重复 owner 检查；HLS `6736c24` / `83fee59` 已简化续期和生命周期。
4. 后续需求：统一授权增加有上限的外部请求，不把业务 deadline 塞进 TCP/UDP transport。
5. 删除正确性：否。会遗漏 DNS 上限、身份回收或分片保留；已有 I/O 错误不能替代无输入或缓存过期。
6. 简化收益：泛化或合并不会减少真实时间事实，反而需要 policy/callback；没有每帧新建 timer。
7. 风险/结论：保留，不改变 GB UDP 任意收到数据即刷新空闲的约定。

### 6. 注册表、授权与目标解析

证据：`media/core/stream_registry.cpp:23`、`media/core/session_registry.cpp:30`、`:46`、`media/http/http_target.cpp:8`、`media/http/signaling_verify.cpp:52`。

1. 真实问题：运行 ID ownership、旧对象身份清理、控制请求 destructive take、一次性认领和严格 URL 解析。
2. 重复性：stream generation、receiver/sender 登记和 HTTP resource 身份不是同一槽位；take 与带 expected 对象的 remove 分别服务控制动作和迟到 cleanup。
3. 历史原因：`4bd3b66` 去掉旧名称/运行 ID 双重登记；`3e99476` 去掉 closing 等待协议；`2bde22f` 合并重复授权和 HTTP target 解析。
4. 后续需求：统一信令授权和 HLS 独立持续身份均为正式能力，不是历史控制令牌的兼容层。
5. 删除正确性：否。可让旧对象删掉新登记、重复使用入口令牌，或绕过严格路径预检。
6. 简化收益：生产只有一份 verify 和 target helper；没有值得新增通用 registry/base 的净收益。
7. 风险/结论：保留。HLS 分片缓存、HTTP chunk queue 和独立播放身份不作为通用 transport queue 的重复实现。

## 本轮保留的边界与未处理项

停止推流后的旧输入迟到登记只记录：例如 `rtmp_session::on_publish()` 在 `verify_stream()` 返回后才调用 `add_receiver_session()`（`media/rtmp/rtmp_session.cpp:253`）。信令认领与媒体登记之间仍有窗口，停止动作可能早于登记。本轮没有注入复现或修复此已接受问题，也没有恢复关闭中槽位或跨 worker 完成通知。

GB UDP 无效报文刷新 idle、SSRC 更新的异步 204、receiver 删除的异步 204，以及完整一次性 token/URL 日志全部按既定设计保持。它们不作为新的待修复 finding。

WHIP 重叠 shutdown 仅作为已核实调用重叠记录，不建议增加 terminal flag。若未来要求改变进程退出或停止后重新启动整个服务，需单独评审，而不能在此次 HLS 修复中删去停止机制。

阶段三没有生产精简提交：满足“删除后所有有效路径仍正确”的候选为 0。实际删除状态/接口均为 0；没有为了完成审计制造重构。

## 本轮验证结果

| 验证 | 实际结果 |
| --- | --- |
| 正常 C++ configure/build | GCC 16.0.1、C++23、RelWithDebInfo、Boost 1.92 静态、FFmpeg SDK `/home/gyl/ffmpeg901`，完整 `cmake --build build -j12` PASS |
| 正常 CTest | 32/32 PASS，174.42 秒，包括 stream_auth、HLS、worker fanout、媒体生命周期、WebRTC auth、idle/accept/control 回归 |
| Go | `go test ./...`、`go vet ./...`、`go test -race ./...` 全部 PASS；Go 生产修改 0 |
| ASan + UBSan | 项目 `MEDIA_SERVER_SANITIZER=address` 同时启用 address/undefined；worker_sink_fanout、hls_segmenter、media_lifecycle、signaling_verify、stream_auth 5/5 PASS，34.31 秒；启用 leak detection 和 halt_on_error |
| 自行审查 | 规范轴未发现违规；需求轴确认授权顺序，提示的 source-end 窗口经主代理代码核对，不新增机制；所有 diff/check PASS |

默认 `/usr/bin/c++` 指向另一套非 PIE 默认的 GCC 16.2，初次链接第三方静态库发生 PIE relocation 环境错误。按仓库已有构建说明明确选择 `/usr/bin/gcc-16`、`/usr/bin/g++-16` 后重新完整构建通过；该链接错误不算 RED。

本机原 Boost.Context 是 fcontext，不能直接用于项目 ASan guard。使用已有 `/home/gyl/Downloads/boost_1_92_0`，在本轮独立临时前缀构建静态 ucontext 与 sanitizer 库，保留项目兼容性检查和 sanitizer 配置。没有替换系统 Boost、修改生产 CMake 或声称所有第三方依赖均已插桩。

正常测试 CLI 为 `/home/gyl/bin/ffmpeg`。本轮没有重跑无关容量/性能套件，也没有声称 Sanitizer 全量 CTest；上述五项就是实际插桩范围。原始构建、RED、测试证据位于 `/tmp/media_server_hls_audit_viHcza/`，不提交原始日志。

提交仅在本地分支保存；按本轮要求不自动 push。
