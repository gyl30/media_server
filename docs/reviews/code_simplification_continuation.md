# 代码简化续审：函数级冗余判断与成员收口

## 一、总结与结论

本轮在 `38364aa` 全协议时序审计（R1/R2 已由 `1e3885e` 实施，V1 暂不处理）的基础上，继续逐函数检查网络层、媒体核心、RTSP/RTMP/GB28181/HTTP-FLV/HLS/WebRTC 会话和共享派生输出，寻找**同一同步区间内已被前置条件证明的重复判断**、只用一次的成员和无作用的包装。

- **实际简化**：12 个独立提交，15 个文件，`+32 / −89`，净删 57 行生产代码；没有修改测试、Go signaling 或 `third/**`。
- **删除状态字段**：2 个（`tcp_listener::port_`、`tcp_listener::bind_address_`，改为 `startup()` 参数）。没有新增任何状态、回调、函数或抽象。
- **删除冗余判断**：10 处条件（入口重复判空/终态检查 7 处、恒为初值的重置 2 处、被后续共同检查覆盖的分支 1 处），另合并 2 组动作相同的相邻分支体、1 组重复的 `if (stream_)`。
- **删除包装层**：1 处无作用的作用域块和临时变量（`take_sender_session`）。
- **是否降低复杂度**：是，但幅度很小。删掉的都是读代码时需要额外确认"这里为什么还要再判一次"的分支；控制流、错误传播和关闭顺序没有变化。
- **拒绝/保留的候选**：13 项（见第三、六部分），其中 5 项经核实是真实的迟到完成或协议阶段保护，必须保留。
- **协议行为影响**：无可观察变化（网络响应、错误码、日志、关闭顺序、source generation 均不变）。每项的不变量依据见第三部分。
- **测试**：最终 HEAD 全量串行 CTest 56/56 通过；每个提交都做了编译和定向回归；改公共基础设施的提交后跑过全量。UBSan 定向结果见第五部分。
- **值得下一轮检查的问题**：1 项 VERIFY——RTSP 播放子会话在 `on_frame` 出错后先于父会话关闭时，迟到的 PLAY 请求可能经 `stream_current()` 访问已置空的 `stream_`（仅源码推导，未复现）。

结论：当前代码整体已经很简单，本轮剩余的可证明冗余集中在"调用方已检查、被调方再检查"这一类小分支。**没有发现值得进行结构性重构的地方**，建议后续不再对同一范围做全仓精简扫描。

## 二、项目历史恢复

### 2.1 来源

| 来源 | 内容 |
|---|---|
| `docs/project_decisions.md`（主仓库本地资料，未进 Git） | D01–D12 决策速查、生命周期与端口池终裁、问题台账、不要重复提案表 |
| `docs/development_conventions.md`（同上） | 提交标题、include 顺序、类成员布局 |
| `docs/reviews/protocol_timing_simplification_audit.md`（`38364aa`） | 全协议时序、状态字段 KEEP 表、R1/R2/V1 |
| `docs/media_core_architecture.md`、`docs/webrtc_transport_audit.md` | source generation + push sink 模型；WebRTC 不提取共同 transport |
| Git 历史（约 120 个提交） | 下表各项的提交依据 |
| 当前源码（`1e3885e`） | 本报告所有 file:function 依据 |

`README.md`、`CLAUDE.md` 在仓库中不存在，已跳过。

### 2.2 当前架构与业务范围（来自源码与 D01/D02）

输入 RTMP、RTSP（推流/拉流）、GB28181（UDP/TCP）、WHIP；输出 RTMP、RTSP、HTTP-FLV、HLS、WHEP、GB28181。`media_stream` 即一个 source generation，固定 tracks，push sink；跨 worker 按目标 worker 分组 drain、有界队列。AAC→Opus 与 MPEG-PS 按 generation 共享，每个 viewer 的网络会话独立。

### 2.3 已放弃的旧方案（来自 `project_decisions.md` §10）

`media_reader`/history/GOP/cursor/discontinuity、运行时 track 变化、反向关键帧请求、端口池 bind 重试、共享完整 RTP/SRTP、通用 `webrtc_transport`、批量 `started_/closed_/sticky error`、transport 自行重试、旧控制面注册/心跳。本轮没有触碰其中任何一项。

### 2.4 此前已完成的生命周期修复与状态精简（来自 Git）

`17c1904` 统一 shutdown→post→safe_shutdown；`6d10f0e`、`8e9432f`、`b898f2a`、`02d6a50`、`87bc65c` 启动失败交由上层 shutdown；`2a863a4`、`e4187c3` 删除 TCP/UDP stopped；`16ec823` 删除 WHEP shutdown_requested；`72be980` 删除国标 RTP 发送器冗余关闭状态；`3af9496`、`a753d54` RTSP 拉流迟到 DNS/PLAY；`a915cbb`、`afd2cc3` RTMP 启动/关闭时序；`9cd85d3`、`80ba127`、`4abaa83` 取消错误日志与错误回调收口；`1e3885e` TCP 写回调重复判空（R1/R2）。

### 2.5 本轮不重复的工作

R1、R2 已实施；V1（拉流迟到 DESCRIBE/SETUP）按既定决定不处理；端口池不重试（D06）；WebRTC 不提取共同 transport（D03）；GB UDP 发送错误回调中先停 `sender_` 再关闭会话（`4abaa83` 刚收口）。

## 三、逐项审查发现

判断标准：只有当**同一同步执行区间内**，前置条件已经证明后续判断必然成立（或该判断所在函数的全部调用方都已保证），并且两者之间没有 `yield`/`co_await`、`post`/`dispatch`、异步回调或可能改变对象状态的用户代码时，才判为冗余。

### 3.1 已实施

| ID | 文件 / 函数 | 原实现 | 冗余依据 | 改动 | 减少的复杂度 | 行为影响 | 验证 | 结论 |
|---|---|---|---|---|---|---|---|---|
| C1 | `media/net/tcp_transport.cpp` `write(span)` | 判空后转调 `write(vector)` | `write(vector)` 第一步对空数据直接返回 | 删外层判空 | −1 分支 | 无（空数据仍不入队） | tcp_transport 6 场景 + 全量 | REMOVE |
| C2 | `media/core/session_registry.cpp` `take_sender_session` | 作用域块 + 临时变量，出块后返回 | 被移出的 `shared_ptr` 在锁内已空，块结束不会析构会话；`take_receiver_session` 已是锁内直接返回 | 锁内直接返回 | −1 作用域/临时变量 | 无（锁范围对返回值无影响） | media_lifecycle、gb_* + 全量 | SIMPLIFY |
| C3 | `media/rtsp/rtsp_server_connection.cpp` `run_read` | 文本分支内 `result == 0 && consumed == 0` 则关闭 | 紧随其后的共同检查 `consumed == 0 \|\| consumed > size` 以同样动作（关闭+返回、无日志）覆盖该情况，中间无语句 | 删内层分支 | −1 分支 | 无 | RTSP 12 项 | REMOVE |
| C4 | `media/rtsp/rtsp_publish_session.cpp` `schedule_tcp_rtcp` | 入口检查 `write_handler_` 与 `tcp_media_` | 仅两个调用点：`on_tcp_record` 刚使用 `tcp_media_`；定时器回调刚检查二者。TCP 模式下二者只在 `safe_shutdown` 一起清空（UDP 路径清 `write_handler_` 与 `tcp_media_` 互斥） | 删入口检查 | −1 分支 | 无；回调内的终态检查保留（异步边界） | RTSP 13 项（含 ffmpeg TCP 推流） | REMOVE |
| C5 | `media/rtsp/rtsp_play_session.cpp` `prepare_presentation` | 入口清 `track_states_`、`waiting_video_track_`、销毁已有 muxer，`!stream_` 返回 404 | 仅在新建会话上调用一次（`admit_play` 与 `tests/media_lifecycle.cpp`），此前只调用过 `set_shutdown_handler`；成员均为初值，`stream_` 来自已查到的源 | 删入口重置与判空 | −2 分支、−2 无效重置 | 无（404 路径不可达） | RTSP 播放 11 项 | REMOVE |
| C6 | `media/gb28181/gb28181_udp_sender_session.cpp` `schedule_rtcp` | 入口 `!stream_` 返回 | 四个调用点（发送首包、定时器两处、RTCP 写完成）都在同一同步区间刚检查 `stream_`；其间只有 UDP 入队、`rtp_rtcp_report`/`rtp_onsend`，不进入 `safe_shutdown` | 删入口检查 | −1 分支 | 无 | GB/UDP/端口池 12 项 | REMOVE |
| C7 | `media/http/http_flv_session.cpp` `on_frame` | `!output_buffer_.empty() && !enqueue(...)` | `enqueue` 对空数据返回成功；`handle_request` 直接调用 `enqueue`；下一次 `on_frame` 先 `clear()` | 删外层判空 | −1 条件项 | 无 | http_flv_auth、stream_auth（ffmpeg 解码 FLV） | REMOVE |
| C8 | `media/http/hls_http_session.cpp` `read_request` | 入口检查 socket 打开 | 唯一调用方 `response_completed` 刚以同一条件关闭并返回 | 删入口检查 | −1 分支 | 无 | hls_segmenter、stream_auth | REMOVE |
| C9 | `media/rtsp/rtsp_pull_session.cpp` `run` | 解析、连接后各有两个相邻分支：取消/身份不符→关闭；出错→关闭 | 两个分支体完全相同（关闭+返回、无日志） | 合并为一个条件，保留全部检查 | −2 重复分支体 | 无（`3af9496` 的取消与 registry 身份检查原样保留） | rtsp_pull_shutdown、rtsp_pull_config、stream_auth | SIMPLIFY |
| C10 | `media/net/tcp_listener.{h,cpp}` | 构造保存 `port_`、`bind_address_`，`startup()` 使用一次 | 三个调用方（`service.cpp`、GB TCP 收/发）都在构造后立即 `startup()` | 改为 `startup(bind_address, port, error)`，构造只取 io | −2 成员字段、−2 构造参数 | 无 | 全量 56/56（三个监听端口、GB TCP、accept 测试） | SIMPLIFY |
| C12 | `media/ps/mpeg_ps_output.cpp` `startup` | `!source \|\| !supported_tracks(...)` | 唯一调用方 `media_stream::ps_output()` 传 `shared_from_this()`；测试也都经 `ps_output()` | 删 `!source` | −1 条件项 | 无 | gb_*、worker_sink_fanout、media_lifecycle | REMOVE |
| C13 | `media/rtsp/rtsp_play_session.cpp` `safe_shutdown` | 两段 `if (stream_)` 中间只重置门槛 | 中间语句不影响 `stream_` | 合并为一个块 | −1 重复判断 | 无（仍先 `remove_sink` 再记录日志、置空） | RTSP 播放 8 项 | SIMPLIFY |

（ID C11 为下表被拒绝的候选，因此已实施编号不连续。）

### 3.2 拒绝、保留与待验证

| ID | 文件 / 函数 | 候选内容 | 核实结果 | 结论 |
|---|---|---|---|---|
| C11 | `tcp_listener::startup/accept`、`udp_transport::startup/connect` | `error.clear()` 后每条路径都会再给 `error` 赋值 | 确属冗余，但这是输出参数初始化的惯用写法，删掉只省 4 行、不减少任何分支或概念 | NO_CHANGE |
| K1 | `rtsp_publish_udp_session::schedule_rtcp` | 入口检查所有轨道端口 | 定时器若在关闭前已成功完成，回调排在 `safe_shutdown` 之后执行，`send_rtcp` 末尾再次进入 `schedule_rtcp`；入口检查阻止关闭后重新挂定时器 | KEEP（迟到完成保护） |
| K2 | `whep_session::schedule_dtls_timeout` | 入口 `connected()` 检查 | `handle_dtls` 在"之前已连接"时也会调用它，检查可达 | KEEP |
| K3 | `whep/whip_session::handle_packet`、写回调 | `local_port_ == 0` | 关闭后迟到的读完成与写完成依赖它 | KEEP |
| K4 | `gb28181_udp_sender_session` 错误路径 | `sender_->shutdown(); shutdown();` 中前者看似重复 | 父关闭是投递执行，前者让发送器立即停止产出；`4abaa83` 刚收口 | KEEP |
| K5 | `rtsp_pull_session::on_rtp` | 最小长度检查与 `rtsp_pull_media::input_packet` 重复 | `media_` 非空时 `media_stream_` 不一定存在（`startup` 在描述为空时提前失败）；删掉后过短包会让 `input_packet` 返回 false 从而关闭会话，行为改变 | KEEP（需要先证明不可达） |
| K6 | `rtsp_publish_session::safe_shutdown` | 清空 `session_id_`、`descriptions_`、`stream_id_` | 父连接已先解除引用，清空不影响正确性，但删除收益很小、且会让关闭后对象内容不再是终态值 | NO_CHANGE |
| K7 | `rtmp_session::send_callback` | 长度为 0 时跳过 `insert` | 去掉不会出错，但只是写法偏好 | NO_CHANGE |
| K8 | `rtsp_publish_media::start_recording`、`rtsp_pull_media::input_packet` | `if (!x) return false; return true;` | 可写成 `return x;`，纯风格 | NO_CHANGE |
| K9 | `media_stream::set_tracks` | 用 `map<track_id,bool>` 查重 | 风格，无冗余状态 | NO_CHANGE |
| K10 | `idle_timer::stop` | `owner_.reset()` | weak_ptr 不延长生命周期，但重置与回调捕获的副本无关，删除无收益 | NO_CHANGE |
| K11 | `udp_transport::startup` 绑定失败自关 socket | 按"失败交给上层 shutdown"可删 | 各调用方随后都会关闭 transport，但会把失败 socket 的释放推迟到会话关闭，且涉及 D06 端口池测试契约；收益小 | NO_CHANGE |
| K12 | `rtmp_play_session::on_frame` | `shutdown(); end_handler_();` | RTMP 父会话的协议回调在子会话关闭后不会再访问子对象（`publish_ \|\| play_` 判断），没有缺陷；删除会改变停止产出的时机 | KEEP |
| V2 | `rtsp_play_session::on_frame` 出错路径 | 先投递子会话关闭，再投递父会话关闭 | 两个投递之间若处理到已排队的第二个 PLAY：`on_play` 通过 Session 与路径检查后调用 `stream_current()`，访问已置空的 `stream_`。`on_describe` 在 SETUP 后会提前返回，`on_setup` 先判 `!stream_`，均安全。仅源码推导，未复现，触发需要客户端在出错瞬间重复发送 PLAY | VERIFY（不在本轮修改） |

## 四、实际代码修改

基线 `1e3885e20b0bb10ad0f5f396b2d627bd1e02e661`，分支 `claude/code-simplification`。

| commit | 文件 | +/− | 内容 | 为什么更简单 | 删除的字段/分支/层次 | 新增状态或抽象 |
|---|---|---|---|---|---|---|
| `7b1fce6` | tcp_transport.cpp | 0/4 | span 重载不再判空 | 判空只在一处 | 1 分支 | 无 |
| `0303b0e` | session_registry.cpp | 15/18 | 锁内直接返回 | 与 receiver 版本一致 | 1 作用域/临时变量 | 无 |
| `3f7b629` | rtsp_server_connection.cpp | 0/5 | 读循环只由共同检查结束 | 结束条件只有一处 | 1 分支 | 无 |
| `8d69820` | rtsp_publish_session.cpp | 0/4 | RTCP 定时入口不再检查 | 终态检查只留在异步边界 | 1 分支 | 无 |
| `8fbdf17` | rtsp_play_session.cpp | 0/12 | 不再重置新建会话 | 去掉不可达的 404 与无效重置 | 2 分支、2 重置 | 无 |
| `5ea776b` | gb28181_udp_sender_session.cpp | 0/5 | RTCP 定时入口不再检查 | 同 C4 | 1 分支 | 无 |
| `a3a1e28` | http_flv_session.cpp | 1/1 | 空数据交给 enqueue | 空数据语义只在 enqueue | 1 条件项 | 无 |
| `30971c8` | hls_http_session.cpp | 0/4 | 读取前不再检查 socket | 检查只在调用方 | 1 分支 | 无 |
| `5da30ee` | rtsp_pull_session.cpp | 2/12 | 合并终止条件 | 一处退出路径 | 2 分支体 | 无 |
| `64a562a` | tcp_listener.h/.cpp、service.cpp、GB TCP 收/发 | 12/19 | 地址端口改为启动参数 | 对象只持有 acceptor | 2 字段 | 无 |
| `a5b3e4e` | mpeg_ps_output.cpp | 1/1 | 不再判断源为空 | 去掉不可达条件 | 1 条件项 | 无 |
| `07702f0` | rtsp_play_session.cpp | 1/4 | 关闭只判断一次源 | 一个块完成解绑与置空 | 1 重复判断 | 无 |

`git diff --stat 1e3885e..07702f0`：15 files changed, 32 insertions(+), 89 deletions(-)。行数只作参考，主要收益是每处调用关系少一次"为什么还要再判"的确认。

## 五、测试结果

构建（独立构建目录，未触碰主仓库与 Pi 产物）：

```text
PKG_CONFIG_PATH=/home/gyl/ffmpeg901/lib/pkgconfig cmake -S . -B /tmp/media-server-claude-simplify-build \
  -DCMAKE_C_COMPILER=/usr/bin/gcc-16 -DCMAKE_CXX_COMPILER=/usr/bin/g++-16 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON -DMEDIA_SERVER_TEST_FFMPEG=/home/gyl/bin/ffmpeg
cmake --build /tmp/media-server-claude-simplify-build -j8
```

子模块 `third/ada`（v4.0.0）、`third/libsrtp`（v2.7）在 worktree 中从主仓库本地检出同一提交，未修改内容。

| 范围 | 版本 | 结果 |
|---|---|---|
| 基线全量串行 CTest | `1e3885e` | 56/56 PASS（258 s） |
| C1/C2 后全量串行 CTest | `0303b0e` | 56/56 PASS |
| C10 后全量串行 CTest | `64a562a` | 56/56 PASS |
| 最终全量串行 CTest | `07702f0` | 56/56 PASS（258 s） |
| 每个提交的定向 CTest | 各提交 | 全部 PASS（数量见第三部分验证列） |
| 编译警告 | 各提交 | 0（项目 `-Werror`） |
| UBSan 定向（`MEDIA_SERVER_SANITIZER=undefined`） | `07702f0` | 36/36 PASS（52 s），`runtime error` 0 条 |
| ASan | — | NOT_RUN：项目文档记载 Boost.Context 配置阻断，本轮改动不涉及内存所有权 |
| TSan | — | NOT_RUN：改动不涉及跨线程访问 |
| Go test | — | NOT_RUN：未修改 signaling |
| 真实浏览器 / 跨机 / 厂商国标 | — | NOT_RUN：本轮为纯控制流精简；ffmpeg 真实推拉流由 stream_auth、rtmp_decode、input_idle 覆盖 |

UBSan 定向范围（`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`，独立构建目录 `/tmp/media-server-claude-simplify-ubsan`）：tcp_transport 6 场景、udp_transport、media_lifecycle、worker_sink_fanout、gb_* 与 gb28181_ps_receiver、rtsp_*（含 rtsp_stop_*、rtsp_pull_*、rtsp_publish、rtsp_play_auth）、rtmp_stop_*、rtmp_play_auth、http_flv_auth、hls_segmenter、media_port_pool_*、stream_auth。未纳入 `accept_recovery`：问题台账 F3 记载其在 UBSan 下因真实 EMFILE 触发 sanitizer 内部失败，与本轮改动无关。

## 六、保留的设计

以下复杂性经本轮再次核实后仍然必要，未做改动：

- **RTSP PLAY 阶段**：`playing_` 表示 PLAY 已被接受；`stream_` 在 DESCRIBE 前就存在，不能互相替代。
- **RTMP Play.Start 顺序**：`prepare()` 暂存初始化包，`rtmp_server_start` 之后 `startup()` 再发送。
- **自然关键帧等待**：各输出的 `waiting_video_track_`（RTSP/RTMP/FLV/WHEP/GB 发送器）与 PS/HLS 的 `waiting_for_key_frame_`。
- **异步取消与迟到回调保护**：拉流 resolve/connect 后的取消与 registry 身份检查（C9 只合并分支体）；GB TCP 建立后的取消检查；UDP 推流 `schedule_rtcp` 入口（K1）；WebRTC `local_port_` 终态（K3）；各定时器回调内的终态检查。
- **source generation**：`stream_registry` 按对象身份删除、RTSP 播放 `stream_current()`、HLS 按源对象身份重建分片器。
- **HLS 分片代际隔离**与 `ended_at_` 保留窗口。
- **GB28181 PS topology**：pending/announced 拓扑与 codec/config/stream index 三类信息。
- **ICE / DTLS / SRTP 阶段**：`remote_endpoint_`、`dtls_`、`srtp_` 与三个定时器分别表达独立事实。
- **worker ownership**：所有 shutdown 仍投递到 owner worker 执行 `safe_shutdown`。
- **session identity**：`session_registry` 的按对象删除与 `stopping_`。

## 七、仍待验证的问题

- **V2**（第三部分）：RTSP 播放子会话因 `on_frame` 出错先关闭后，排队中的第二个 PLAY 可能访问空 `stream_`。建议先用真实 TCP 时序测试确认可达性；若可达，最小方案是在 `on_play` 开头复用已有的 `!stream_` 判断（与 `on_setup` 一致），或去掉 `on_frame` 出错路径中子会话自己的 `shutdown()`，交由父会话逐层关闭。两种方案都会影响日志或停止产出的时机，需要单独评审，不属于本轮"行为不变的精简"。

V1（拉流迟到 DESCRIBE/SETUP）维持原决定，本轮没有新证据。

## 八、最终建议

- **可以接受**：C1–C10、C12、C13 均为同步区间内可证明的重复判断或只用一次的成员，行为不变，逐个提交可独立回退。
- **建议再次检查**：C10（接口签名变化，涉及 5 个文件）与 C5（依赖"只在新建会话上调用一次"这一调用约定），Review 时请确认没有计划中的新调用方。
- **建议回退的候选**：无。
- **不必继续投入的方向**：再做一次全仓精简扫描；把 `error.clear()`、`return x ? true : false` 一类惯用写法逐个改掉；把 WebRTC 两个会话或 RTCP 定时器抽成公共组件（D03 及上一轮审计已否决）。
- **V2** 需要先有时序复现，再决定是否修改。
