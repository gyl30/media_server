# RTSP 播放 V2 生命周期时序验证

日期：2026-10-13

## 1. 最终结论

| 项目 | 结论 |
| --- | --- |
| 结论分类 | **PROVEN_UNREACHABLE**（有前提，见第 3 节和第 7 节） |
| V2 是否可达 | 在当前代码和 Boost 1.92 调度实现下不可达 |
| 是否有真实生命周期缺陷 | 没有发现 |
| 是否修改生产代码 | 否 |
| 是否新增测试 | 是。`tests/protocol_play_auth.cpp` 新增 6 个真实 TCP 场景，提交 4690a16 |
| 是否建议合并 | 建议合并测试和本报告 |
| 是否仍有未决问题 | 结论限定在当前单线程 worker、Boost.Asio 1.92 配置和已检查的调度实现下成立，不是独立于 Asio 实现的协议级不变量；见第 7 节 |

V2 原本假设的时序是：子会话 `safe_shutdown` 已执行，父连接的 `safe_shutdown` 还没执行，中间插入第二个 PLAY。

这个时序不会出现，原因是：

- 子、父的两个关闭 handler 投递到同一个单线程 `io_context`，并且在同一同步区间内相邻投递。
- PLAY 只能在父连接读协程恢复后才被处理。
- 读协程恢复所需的读完成事件，只能由 reactor 在两个关闭 handler 之前或之后入队，不会插在两者中间。

真实 TCP 测试与这个推导一致。把父连接关闭人为推迟到 reactor 之后（只是临时实验，没有提交），同一测试会在 `stream_current()` 处段错误，说明测试能检出 V2。

## 2. 代码基线与历史

- BASE_HEAD：`7788320af210ab63939c84b6fdcb4e7518674caa`（main，代码简化成果已合并）。
- 分支：`claude/rtsp-play-v2`。
- worktree：`/home/gyl/example/media_server-claude-rtsp-v2`。
- 子模块和主仓库是同一提交，没有改动。

V2 的来源：

- `protocol_timing_simplification_audit.md` 第 90 行列为 VERIFY，当时只有源码推导。
- `code_simplification_continuation.md` 沿用了这一结论，建议先用真实 TCP 时序确认可达性，再决定是否修改。

V2 与已经修复的问题不同：

- 已修复的是确定可达的重复判断、回调清理问题。
- V2 是跨两个 handler 的时序假设，是否成立取决于调度顺序。

## 3. 真实事件时序

标记说明：【源码】表示由源码保证；【测试】表示由本轮测试观察到；【推测】表示未证实。

### 3.1 前提条件【源码】

- `worker_context` 用 `io_context io_{1}` 创建，`media/net/worker_context.h`。并发提示为 1，Asio 会设置 `one_thread_` 和 `assume_continuation_`。
- `worker_pool::run()` 为每个 worker 只开一个线程调用 `run()`，`media/net/worker_pool.cpp:35-44`。
- `admit_play` 用父连接的 `worker_` 构造播放子会话，`rtsp_server_connection.cpp:390`。子、父的 `shutdown()` 都投递到同一个 `io_context`。
- `on_frame` 的失败分支依次同步调用 `shutdown()` 和 `shutdown_handler_()`，`rtsp_play_session.cpp:100-101/114-115/129-130/146-147`。两者之间没有 yield，也没有回调。
- `shutdown_handler_` 就是 `owner->shutdown()`，`rtsp_server_connection.cpp:401`，即再 `post` 一次 `safe_shutdown`。

### 3.2 `on_frame` 在哪里执行【源码】

`worker_sink_dispatcher::publish` 有两种情况：

- 源 worker 等于播放 worker：在发布方当前的 handler 中同步调用 `on_frame`。发布方可能是推流连接的读协程，也可能是其他 handler。
- 源 worker 不等于播放 worker：先 `post` 一个 `drain` 到播放 worker，再在 `drain` handler 中调用 `on_frame`。

两种情况下，`on_frame` 都运行在播放 worker 的某个 handler H 里，并且 H 不是该播放连接自己的读协程：

- 播放连接不能同时推流（`publish_session_` 和 `play_session_` 互斥）。
- `add_sink` 不会同步回放缓存帧。

### 3.3 投递顺序【源码，Boost 1.92 `asio/detail/impl/scheduler.ipp`】

- H 执行期间，`post(op1 = child safe_shutdown)` 和 `post(op2 = parent safe_shutdown)` 都经过 `post_immediate_completion`，因为 `assume_continuation_` 为真，二者进入当前线程的 `private_op_queue`（第 357-377 行）。
- H 结束时，`work_cleanup` 把私有队列整体接到主队列尾部（第 100-104 行），op1 和 op2 在主队列中相邻。
- 网络读完成只在 reactor 任务（`task_operation_`）运行时产生。`task_cleanup` 把它们接到主队列尾部，然后把 `task_operation_` 放回队尾（第 72-75 行）。
- 单线程下，H 结束之后才可能运行 reactor 任务。所以读完成要么在 op1 之前已经入队，要么在 op2 之后入队，不会插在 op1 和 op2 之间。

### 3.4 由此得到的三种顺序

1. **读完成先于 H。**
   - 【源码】第二个 PLAY 在子会话完整时被处理：`playing_` 为真，回复 200，然后才发生媒体失败。
   - 【测试】`play_replied` 和 `play_cross_worker` 观察到这种顺序。
2. **读完成在 H 之后，reactor 先于 op1 运行。**
   - 【源码】读完成排在 op2 之后。
   - 【源码】op1：子会话 `safe_shutdown`。
   - 【源码】op2：父连接 `safe_shutdown`，把 `play_session_` 置空，并投递 `transport` 关闭。
   - 【源码】读协程恢复后，`rtsp_server_input` 回调 `play_callback`；因为 `play_session_` 为空，返回 -1 并调用 `shutdown()`，不会访问子会话。
3. **op1、op2 先于 reactor 运行。**
   - 【源码】op2 投递的 socket 关闭会让挂起的读操作以中止错误结束，第二个 PLAY 不会被解析。

【测试】`play_buffered` 和 `play_combined` 只能分辨出属于顺序 2 或顺序 3，两者都观察到"服务端不回复第二个 PLAY，连接关闭"。测试没有区分具体是 2 还是 3，因为父连接的 `play_callback` 拒绝日志是 debug 级别，测试没有开启。

### 3.5 其他关闭来源【源码】

子会话的 `shutdown()` 只有以下来源：

- `on_frame` 失败：紧接着关闭父连接。
- 父连接 `safe_shutdown`：同一个 handler 内先把 `play_session_` 置空。
- `admit_play` 中 `prepare_presentation` 失败：该子会话还没有挂到父连接上。

所以不存在"子会话已关闭、父连接还持有它并继续处理请求"的其他路径。

### 3.6 释放【源码 + 测试】

- 子会话 `safe_shutdown` 依次执行：`remove_sink`、销毁 muxer、清空两个 handler，从而释放对父连接的引用。
- 父连接 `safe_shutdown` 依次执行：把 `play_session_` 置空、关闭 transport。
- 【测试】每个场景结束后：
  - 父连接 `weak_ptr` 已失效。
  - RTSP muxer 存活数为 0，销毁数为 1。
  - `stream_registry` 中仍是原来的 source generation。

## 4. 测试设计与原始证据

所有场景都复用 `admission(false, scenario)`。使用的组件：

- 真实 loopback TCP。
- 生产 `rtsp_server_connection` 和 `rtsp_play_session`。
- ireader RTSP parser。
- 真实 HTTP 鉴权回环。
- 真实 `media_stream` 和 `media_frame`。

测试流程：

- 轨道是 G.711A、8 kHz、单声道。
- 客户端依次发送 DESCRIBE → SETUP（`interleaved=0-1`）→ PLAY，每一步都要求服务端返回 200。
- 失败帧的 `pts_ns` 和 `dts_ns` 都是 20000001，不是毫秒对齐，会进入 `on_frame` 的时间戳精度失败分支。日志 `rtsp play audio timestamp precision unsupported ...` 证明命中了目标路径。

| 场景 | 排列方式 | 断言 |
| --- | --- | --- |
| `play_normal` | 发送合法帧（20 ms） | 收到 `$` 通道 0 的 interleaved RTP；source generation 不变 |
| `play_replied` | 第二个 PLAY 收到 200 之后，再发送失败帧 | 连接关闭，之后没有新的 RTSP 回复；muxer 已销毁；generation 不变 |
| `play_buffered` | 在同一个 handler 内：同步写入第二个 PLAY，然后立即发送失败帧 | 第二个 PLAY 没有回复；连接关闭；muxer 已销毁；generation 不变 |
| `play_combined` | 同上，但第二、第三个 PLAY 在一次写入中 | 同上 |
| `play_after_failure` | 先发送失败帧，再异步写入第二个 PLAY | 同上 |
| `play_cross_worker` | 源在另一个线程的 worker 上，失败帧经 `drain` 送达 | 第二个 PLAY 要么收到 200，要么没有回复；两种情况都必须安全释放 |

所有场景还检查：父连接最终释放；muxer 创建 1 次、销毁 1 次。

`play_buffered` 中，同步写入完成时数据已经进入服务端内核缓冲区。由于仍在同一个 handler 内，reactor 无法收集服务端的读完成。随后同步调用 `publish` 触发 `on_frame` 失败。这正是 V2 所需的"PLAY 已就绪、读协程尚未恢复"的时序。

`play_cross_worker` 中，读完成相对 `drain` 的先后不受控制。50 次运行全部是"PLAY 先回复、然后媒体失败"，所以跨 worker 情况下的顺序 2 和 3 没有直接观察到。由于子、父关闭始终在同一个播放 worker 上进行，这两种顺序的安全性仍由 3.3 节的源码推导保证。

### 灵敏度实验（没有提交）

临时修改 `on_frame` 的时间戳失败分支：保留子会话 `shutdown()`，把 `shutdown_handler_()` 改成 20 ms 定时器之后再调用，人为让读完成插到子、父关闭之间。

- `protocol_play_auth_tests rtsp` 在 `play_buffered` 中段错误，退出码 139。
- gdb 调用栈：`rtsp_play_session::stream_current` ← `on_play`（`rtsp_play_session.cpp:389`）← `play_callback` ← `rtsp_server_play` ← `rtsp_server_input` ← `run_read`。

这与 V2 的推导完全一致，说明新测试能检出 V2 时序。该修改已经用 `git checkout` 恢复，并重新编译。

## 5. 最小修复及对比

没有实施修复，因为 V2 不可达。

- 候选 A（在 `on_play` 入口检查 `!stream_`）：在当前时序下只会保护一个不可达的状态，属于为理论场景增加的防御，不采用。
- 候选 B（失败路径只关闭父连接）：会让子会话在父连接清理前继续接收帧，改变停止输出的时机；而且它并不是修复 V2 的必要条件，不采用。

## 6. 回归结果

以下都在 `4690a16` 上实际执行。构建目录 `/tmp/media-server-claude-rtsp-v2-build`，gcc-16，RelWithDebInfo。

| 项目 | 命令 | 结果 |
| --- | --- | --- |
| 全量构建 | `cmake --build <build> -j12` | 成功，0 条 warning |
| 定向测试 | `ctest -R rtsp_play_auth` | PASS（11 个 RTSP 场景） |
| 重复运行 | `protocol_play_auth_tests rtsp` × 50 | 50/50 PASS |
| 完整 CTest | `ctest --test-dir <build> --output-on-failure`，串行 | 56/56 PASS，259.36 秒 |
| 灵敏度实验 | 见第 4 节 | 修改后段错误，恢复后 PASS |

没有运行的项目：

- UBSan、ASan：本轮没有修改生产代码。ASan 受 Boost.Context 限制，项目已知。
- TSan：没有跨线程的代码改动。
- 跨机或真实客户端：本轮不涉及。

## 7. 最终建议

**PROVEN_UNREACHABLE**，生产代码保持不变。

这个结论依赖以下前提。将来任何一条被打破，都需要重新评估 V2：

1. 每个 worker 的 `io_context` 只由一个线程运行（`io_context{1}`，`worker_pool` 每个 worker 一个线程）。
2. 播放子会话与父连接使用同一个 `worker_context`。
3. `on_frame` 失败分支中，`shutdown()` 和 `shutdown_handler_()` 在同一同步区间内相邻调用，并且 `shutdown_handler_` 是同步 `post` 父连接关闭，中间没有定时器、额外 post 或跨 worker 跳转。
4. 子会话只会因为 3.5 节列出的三种来源而关闭。
5. Boost.Asio 调度器把同一 handler 内的投递整体接到队尾，读完成只在 reactor 任务中入队。
   - Boost.Asio 1.92 文档已经说明：`concurrency_hint=1` 时，handler 内投递的操作先进入线程本地队列，再批量转移到主队列。
   - 本报告据此并结合 `scheduler.ipp` 和 epoll reactor 的源码，推出读完成不会插在子、父两个关闭 handler 之间。
   - 这一推导只覆盖当前配置和已检查的实现，不能推广到所有 Asio backend、配置和版本。

因此结论的准确范围是：在当前单线程 worker、Boost.Asio 1.92 配置与已检查的调度实现下，V2 不可达。该结论不是独立于 Asio 实现的协议级不变量。

`play_buffered` 已经通过灵敏度实验（第 4 节）：人为延迟父连接关闭后，它能检测到目标危险排列。但这并不保证将来所有调度变化都一定会被这个测试捕获。
