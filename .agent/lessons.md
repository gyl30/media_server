# 已核实的开发经验

下文测试路径属于所列历史提交；旧测试已清理，使用 `git show <提交>:<路径>` 查阅，历史修复与验证事实保持原记录。

“出现次数”只给出可核实的类别或测试/报告次数，未记录的线上频率不猜测，不把提交数当故障次数。历史实现被后续决定覆盖时保留演进；源码位置以 `813b4f1` 为核对点。经验以引用提交、源码与回归为依据，未跟踪的本地历史资料只作补充。

## L01：重复状态会制造第二份资源真相

- 问题：`started_`、关闭/延迟关闭标志、`recording_`、`published_` 曾与一次启动、tracks 和资源存在性重复。
- 原因：每个字段都需要与初始化、错误和清理同步，扩大了有效状态组合。
- 修复：删除一次性启动状态；RTSP 录制与 WHIP 发布使用固定 tracks 表达准备事实，关闭依据所属资源。当前 RTSP `playing_` 和 GB `rtcp_reporting_started_` 仍保留，不能照此经验批量删 bool。
- 相关提交：`ecb03b7`「收口一次性会话启动状态」、`e30da50`「删除 RTSP 延迟关闭状态」、`3d4b1c8`「删除 RTSP 发布录制状态」、`0c9de93`「统一媒体源注册语义」。
- 出现次数：可核实启动、关闭、录制、发布四类清理；线上故障次数未记录。
- 是否升级为约束：是，[C02](constraints.md)；清理收益不是证明每个旧字段都造成过崩溃。

## L02：DNS 成功恢复不等于旧接收器仍有效

- 问题：RTSP Pull 关闭后迟到 DNS 恢复可能继续连接，影响同 stream 的替换会话。
- 原因：取消请求与 resolve 成功可以交错，只检查 error 不能表达当前 registry identity。
- 修复：resolve/connect 后检查 `yield.cancelled()`、接收器 identity 和 error，让旧协程退出。当前见 `rtsp_pull_session::run()`；`tests/rtsp_pull_shutdown.cpp` 用受控 resolver 覆盖关闭及替换。
- 相关提交：`3af9496`「阻止已关闭 RTSP 拉流被迟到解析重新建立」，后续 `5da30ee` 合并同一终止判断。
- 出现次数：一类已修复问题，回归有关闭/替换两个场景；线上频率未记录。
- 是否升级为约束：是，[C04](constraints.md)；不据此给所有回调增加通用 guard。

## L03：父清理先于 TCP 子关闭，PLAY 仍可迟到

- 问题：Pull 的父 `safe_shutdown()` 已 reset `media_`，成功可读的 PLAY 响应仍进入 ireader 回调，原代码直接访问空媒体对象。
- 原因：TCP `shutdown()` 自己也是异步投递，取消 socket 不是父清理时同步完成的屏障。
- 修复：`play_callback()` 在实际使用前检查 `media_`，为空返回解析错误，沿现有读流程退出；不添加 closed/started。真实 TCP 回归覆盖正常与迟到 PLAY、registry 移出、peer EOF 和 weak lifetime 释放。
- 相关提交：`a753d54`「修复 RTSP 拉流迟到 PLAY 回调空指针」，文件为 `rtsp_pull_session.cpp`、`tests/rtsp_pull_shutdown.cpp`。
- 出现次数：一类可达缺陷；受控回归两个 PLAY 场景，不是线上崩溃概率。
- 是否升级为约束：是，[C04](constraints.md)。本地未跟踪的旧 `docs/project_decisions.md` 中 F4“待验证”已被该提交覆盖，不作为当前状态依据。

## L04：移除 sink 后仍可能收到已排队媒体

- 问题：RTMP 播放关闭窗口中，迟到 frame 或保留的回调可能继续访问父 context/transport。
- 原因：remove 不撤销既有 fanout snapshot；只清 muxer 而保留父回调也会延长所有权。
- 修复：播放子清理时清空 packet/end handler；父发送边界使用当前 context/transport，sink 用 `stream_` 终态跳过迟到 frame。`tests/protocol_play_auth.cpp` 覆盖 late frame 和回调释放。
- 相关提交：`afd2cc3`「修复 RTMP 关闭窗口的迟到媒体输出」、`a915cbb`「修复 RTMP 启动与工作线程停止交错」。
- 出现次数：已核实迟到输出与启动/worker stop 两类问题；线上频率未记录。
- 是否升级为约束：是，[C04](constraints.md)；不能把必要保活统一改成 weak capture。

## L05：GB sender 的 PS 回调与传输资源寿命不同

- 问题：排队的 PS frame/end、重复关闭及 worker stop 涉及父 sender、RTP 子组件、timer 和端口，局部 closed bool 不能解释整个所有权链。
- 原因：源分发和网络 completion 各自保活，简单移出 registry 并不结束所有回调。
- 修复：父在 owner 清理，移动/清空 `stream_` 表达终态，关闭子组件和 transport、取消 RTCP、归还 P 并断开回调。`tests/gb_sender_lifecycle.cpp` 检查迟到输出、重复关闭、worker stop、对象释放。
- 相关提交：`d8a2428`「收口国标父子回调保活与停机关闭入口」、`72be980`「移除国标 RTP 发送器冗余关闭状态」。
- 出现次数：一组跨组件生命周期收口，多个受控测试场景；未记录线上次数，不能声称每个清理提交都是独立故障。
- 是否升级为约束：是，[C04](constraints.md)。`rtcp_reporting_started_` 的一次报告启动语义仍独立。

## L06：WHEP 终态保护要随线程模型演进

- 问题：早期 WHEP 跨 worker 关闭直接改 `started_`，与帧回调竞争；旧报告随后把原子 fence 当作固定必需字段。
- 原因：资源清理和帧回调的所属执行器决定保护方式，字段名本身不是生命周期契约。
- 修复：`a5ce7f5` 当时增加 atomic fence；`16ec823` 后将清理放回 owner 并删除该字段。当前 `on_frame()` 用 `srtp_`、关闭用 `stream_`、发送用 `local_port_`，保留终态语义而不恢复旧状态。现有回归为 `tests/whep_lifecycle.cpp` 等。
- 相关提交：`a5ce7f5`「修正 WHEP 跨工作线程关闭竞争」、`16ec823`「精简 WHEP 会话关闭生命周期」。
- 出现次数：一类历史竞争及一次后续模型收口；线上频率未记录。
- 是否升级为约束：是，[C02、C04](constraints.md)；不升级为“必须有 shutdown_requested_”。

## L07：排队和发送完成不是同一件事

- 问题：高并发 WHEP 的发送调度曾由 session 推进，UDP 入队拒绝也容易与真正 socket failure 混淆。
- 原因：buffer ownership、single-flight 和 completion 属于网络层，UDP 丢新包与会话失败的策略不同。
- 修复：先以 async completion 推进 WHEP 写队列，后统一到 UDP transport；入队拒绝不冒充网络 completion。当前 datagram 由 completion 捕获独立保活，shutdown 清 queue 不使在途 buffer 悬挂；`tests/udp_transport.cpp` 覆盖 endpoint、overflow 和关闭。
- 相关提交：`17e068c`「由异步完成回调推进 WHEP UDP 写队列」、`f572de3`「区分 UDP 入队拒绝与实际发送错误」、`e4187c3` 的终态/错误传播收口。
- 出现次数：已核实调度、入队/完成两类演进；高并发故障次数未记录。
- 是否升级为约束：是，[C05](constraints.md)，不扩大为共享 RTP/SRTP 或增大队列。

## L08：返回 403 仍可能已创建未授权资源

- 问题：HLS 一次性入口先调用 `get_or_create()` 再 verify，无效授权最终被拒绝，但已可能创建 segmenter、muxer 并订阅源。
- 原因：只看响应码没有覆盖授权前的副作用。
- 修复：verify 成功后才获取共享 segmenter 和创建独立 play session，保持一次性认领及后续 playlist/segment 行为。`tests/stream_auth.py`、`tests/hls_segmenter.cpp` 检查授权和共享资源。
- 相关提交：`f1a6d49`「修复 HLS 授权前创建分片器」。
- 出现次数：一个确认的入口顺序问题；线上利用或发生次数未记录。
- 是否升级为约束：是，[C06](constraints.md)。

## L09：EMFILE 恢复测试必须证明耗尽真的发生

- 问题：原 accept 测试只等释放后 OPTIONS 成功；RLIMIT 改为 512、不发生 EMFILE 仍会通过。
- 原因：客户端连接失败、backlog 或最终恢复不能证明服务端经历目标错误。
- 修复：捕获服务输出，观察明确 EMFILE 后才释放连接，有限等待进程存活及新 OPTIONS 200，并清理本轮资源。当前 `tests/accept_recovery.py` 已包含此要求，生产 3 秒 accept 重试未改。
- 相关提交：`b4965b8`「强化真实 EMFILE 恢复测试」。
- 出现次数：一项已确认测试假阳性；另外一个 UBSan runtime 在 FD 耗尽下内部 `pipe()` CHECK 失败，属于环境限制，不是已发现程序 UB。
- 是否升级为约束：是，[C07](constraints.md)。旧 F3 待修项已关闭，但 sanitizer 对此极低 FD 场景的覆盖限制仍需单独说明。

## L10：高并发失败需要对照，容量数字需要条件

- 问题：分发重构首轮 1000 WHEP 出现大量 queue-full；仅凭一次失败会误判为重构退化。
- 原因：同机编译/CTest 与媒体负载同时运行是环境混淆项；native 收包、浏览器解码、不同 worker/fixture/网络条件也不能互相替代。
- 修复：历史报告保留原失败，再串行对照旧/新二进制，均恢复健康，没有据此修生产或放宽健康条件。后续工具使用独立凭据、逐 viewer 媒体进展及公共测量窗口；当前性能报告继续区分测试上限与容量边界。
- 相关提交：`90cdd7f` 的媒体分发验证报告、`fb24663` 的凭据/健康检查、`7706ae2` 的测量窗口修正。旧分发报告已由 `10905a9` 清理，可用 `git show 90cdd7f:docs/worker_sink_dispatcher_verification.md` 查阅。
- 出现次数：该历史阶段一轮 1000 失败及其对照复测；不据此统计其他基线失败率，也不认定环境竞争是已精确量化的唯一原因。
- 是否升级为约束：是，[C07](constraints.md)；热点、RSS 或一次 PASS 都不能单独证明优化收益、泄漏或最大容量。
