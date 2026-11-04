# 端口池异常收口与 WHEP SRTP replay_fail 根因（2026-11-04）

基线 `93bb9eb`（`origin/main`），工作区未提交。

## 结论

- **端口池：GO。** 端口范围改为编译期常量并用 `static_assert` 约束，删除不可达的 `throw std::invalid_argument`；分配顺序、成对端口、单例、锁和释放语义不变。
- **WHEP `replay_fail`：根因已确定，GO。**
  - 我们构建 ireader 时没有定义 `OS_LINUX`，`rtp_ssrc()` 因此退回进程全局的 `rand()`。
  - librtmp 每次握手都会执行 `srand(time(NULL))`，随后连续调用 382 次 `rand()`。
  - 如果另一个 worker 上的 RTMP 握手恰好落在 WHEP 打包器初始化视频轨和音频轨之间，PRNG 会被重置到同一状态，两条轨道就会拿到相同的 SSRC、初始序号和时间戳。
  - 视频轨先发出关键帧，用掉这些序号；音频随后用相同的 SSRC 和序号进入同一个 SRTP 会话，`srtp_protect` 返回 `srtp_err_status_replay_fail`（9），会话随即关闭。
  - 修复是在 `cmake/ireader.cmake` 中为 ireader 增加 `OS_LINUX` 编译定义，让 `rtp_ssrc()` 改走 ireader 自带的 `/dev/urandom` 实现。没有修改 `third/`、WHEP 或 SRTP 代码。

## 1. 端口池

| 文件 | 改动 |
| --- | --- |
| `media/net/media_port_pool.h` | `init()` 和构造函数都不再接收参数；删除 `default_media_port_start/end` |
| `media/net/media_port_pool.cpp` | 在匿名命名空间中定义 `media_port_start = 49152`、`media_port_end = 65535`；两条 `static_assert` 分别检查：起始端口为正且为偶数；范围内至少有一对完整端口，并且不超过 `uint16_t`。删除两处 `throw` 和 `<stdexcept>`。原来对奇数起始端口的取偶修正改为由 `static_assert` 要求偶数 |
| `main.cpp` | 改为调用 `media_port_pool::init()` |

合计 3 个文件，+19/−21。

`port_pool_check.cpp` 在 rel、asan、tsan 三种构建下均为 PASS，覆盖：

- 首个分配为 49152，释放后复用；
- 耗尽前共 8192 对端口，RTP 端口全为偶数，RTCP 端口不越界，没有重复分配；
- 耗尽后返回 `nullopt`；
- 重复释放和释放未知端口都不会增加容量；
- 8 线程并发分配和释放时，同一端口不会被两处同时持有；结束后 8192 对端口全部可用。

服务启动由第 5 节的协议矩阵和生命周期测试覆盖。

## 2. replay_fail 调查

### 2.1 历史证据

测试机上共有 4 次失败：`runs8/u9-d-2.2-gso2-r{2,3}`，以及 10-30 的 `runs/m22-asan-b1` 和 `runs/mx-lease-asan-mixed`。它们的特征一致：

- libsrtp 返回 status 9；
- 紧接着出现 `webrtc audio rtp packetize failed codec opus`，会话随后关闭；
- 失败都发生在会话启动阶段，距 SRTP 启动 0.15–1.4 s；
- 会话创建前后 20 ms 内都有多条 `rtmp play` 记录，混合负载带 `--churn rtmp:100`。

### 2.2 逐项排查

| # | 检查项 | 证据 | 结论 |
| --- | --- | --- | --- |
| 1 | `on_packet()` 的实际输出 | 每条轨道各有一个 ireader `rtp_sender`。音频走 `rtp_common_encode`（`rtp-pack.c`），每发一个包 `seq` 加一；视频走 H.264 打包器。`on_packet` 只追加 MID 扩展并重新序列化，不改变 SSRC 和序号 | 单条轨道内的序号严格递增 |
| 2 | `send_rtp()` 是否重复提交 | 每个会话只有一个 `srtp_t`（`ssrc_any_outbound`），按 SSRC 维护重放窗口。同一会话内出现重复的 SSRC+seq，只可能是两条轨道共用了同一个 SSRC | 只有 SSRC 冲突才会导致重复 |
| 3 | ireader 的序号初始化 | `rtp-sender.c:62-64,109-111`：seq、SSRC、时间戳为 0 时都取 `rtp_ssrc()`。`rtp-ssrc.c` 只在定义了 `OS_LINUX`/`OS_MAC` 时读取 `/dev/urandom`，否则直接 `return rand()`。`compile_commands.json` 中的 `rtp-ssrc.c` 没有 `-DOS_LINUX`。出错时没有回退：handler 返回非 0 后，循环在 seq 自增后退出 | 根因所在 |
| 4 | 视频与音频 SSRC 是否重合 | `rtmp-handshake.c:182-192` 的 `rtmp_handshake_random()` 先执行 `srand(timestamp)`，再调用 382 次 `rand()`；`rtmp-server.c:74` 传入的 timestamp 是 `time(NULL)`。同一秒内的每次握手都把全局 PRNG 重置到同一状态 | 跨线程的握手会让两条轨道拿到相同的值 |
| 5 | 启动后 150–300 ms 内的时序 | `whep_session::on_frame` 在第一个视频关键帧之前丢弃所有帧，因此视频总是先发出，并用掉冲突的序号；随后的第一个音频包与之重复。这解释了为什么失败的总是音频轨（AAC→Opus egress 和 Opus 直通都是如此） | 与历史证据一致 |
| 6 | 旧帧、重复投递、重入、生命周期 | 打包器只在会话所在的 worker 上调用，`send_rtp` 不回调打包器；确定性回归在没有任何重投的情况下就能复现同样的失败 | 没有发现其他路径 |

纯视频 WHEP 只有一条轨道，不受影响；只要会话里有两条轨道，无论音频是 Opus 直通、AAC→Opus 还是 G.711，都可能触发。

### 2.3 确定性回归（`rtp_ssrc_check.cpp`）

检查程序直接驱动生产代码 `webrtc_packetizer`（H.264 + Opus 两条轨道）和 `srtp_transport`，并调用真实的 librtmp `rtmp_handshake_s1`。它不读取密钥或 payload，只解析 RTP 头。

- **A（单线程，完全确定）**：在同一秒的 timestamp 下做两次握手，每次握手后各建一个会话，检查两个会话的 SSRC 和序号是否不同。
- **B（并发）**：一个线程每隔 20 µs 做一次握手，主线程依次创建会话，并按生产顺序发送视频关键帧和音频帧，统计视频与音频 SSRC 相同的会话数以及 `srtp_protect` 失败的会话数。

| 构建 | 修复前（A / B） | 修复后（A / B） |
| --- | --- | --- |
| Release，200000 个会话 | FAIL：两个会话的 SSRC 都是 `3be23e95`、seq 都是 56494；B 中 28 个会话 SSRC 冲突、28 个 protect 失败 | PASS / 0、0 |
| ASan，20000 个会话 | FAIL；1981 个冲突、1963 个失败 | PASS / 0、0 |
| TSan，20000 个会话 | FAIL；13886 个冲突、13845 个失败 | PASS / 0、0 |

本地另用 debug 日志版本确认，失败全部是 `webrtc srtp protect failed status 9`，与生产日志相同。插桩构建越慢，两次初始化之间的窗口越宽，冲突率越高，这与历史失败主要出现在 ASan 运行中一致。

### 2.4 修复

```diff
 target_compile_definitions(ireader_media PRIVATE
     NDEBUG
+    OS_LINUX
```

- 编译的 ireader 源文件中，只有 `rtp-ssrc.c` 检查 `OS_LINUX`/`OS_MAC`，ireader 的头文件里没有，所以这个定义不影响其他代码路径。
- `rtp_ssrc()` 改为每次 open/read/close `/dev/urandom`，只在建会话或建轨道时调用，不在逐包路径上。
- librtmp 握手仍然使用 `srand/rand`，但修复后 RTP 的 SSRC、序号和时间戳不再依赖 `rand()`。
- 没有缩小重放窗口、忽略错误、重建会话、延时或重试，也没有改动媒体分发和转码器。

## 3. 定向复现（混合负载 D）

条件与历史失败相同：混合负载 D 2.2×，8 个 worker，相同的素材、CPU 亲和性、客户端，以及 `--churn rtmp:100`。修复前的二进制（`93bb9eb`）与修复后的二进制交替各跑 8 轮。每轮约有 1971 个 WHEP 会话启动（含 AAC→Opus 共享 egress、纯视频观众、源 churn 和观看者 churn），271 次 RTMP play。

| 版本 | 运行 | 结果 | `srtp protect failed` |
| --- | --- | --- | --- |
| 修复前 | 8 | 8 PASS HEALTHY | 0 |
| 修复后 | 8 | 8 PASS HEALTHY | 0 |

在负载层面没有复现：历史发生率是 Release 下 20 次中 2 次，8 轮跑出 0 次在预期之内。因此修复前后都是 0，**不能单独作为修复有效的证据**。根因和修复效果以 2.3 节的确定性回归为准：它在三种构建下稳定复现并区分了修复前后。负载测试的作用是确认修复没有带来回退。

## 4. 验证

修复后的二进制在测试机上运行（`r01-checks`、`r03-verify`）：

| 项目 | Release | ASan+UBSan | TSan |
| --- | --- | --- | --- |
| 本地构建 | 0 warning | 0 warning | 0 warning |
| `port_pool_check` | PASS | PASS | PASS |
| `rtp_ssrc_check`（2.3 节） | PASS | PASS | PASS |
| 协议矩阵 | 32 PASS + 3 XFAIL（35 个场景） | 20 PASS + 2 XFAIL（22 个场景） | 11 PASS + 1 XFAIL（12 个场景） |
| WHEP 边界（Chrome：纯视频、PCMU 降级、音视频；WHIP 源与 Pull 源；DELETE、源停止） | PASS | PASS | PASS |
| 生命周期 WHEP（含源 churn 与观看者 churn） | PASS HEALTHY | PASS HEALTHY | PASS HEALTHY |
| 生命周期 GB UDP | PASS HEALTHY | PASS HEALTHY | PASS HEALTHY |

- XFAIL 全部是严格的 RTSP AAC 时间戳锯齿，与既有规则一致，没有 XPASS。
- 共产生 34 个 sanitizer 文件，全部出自 ASan，内容都是 Boost.Context 的 `makecontext/swapcontext` 提示；没有 ASan、UBSan 或 TSan 报告。
- 所有 `r3-*` 日志中，`srtp protect failed` 和 `packetize failed` 均为 0 次。
- 生命周期结束后，fd 回落到基线以下，没有残留的接收端。
- `clang-format --dry-run` 检查改动文件和两个检查程序，均无输出。

## 5. 未解决风险

- 两条轨道各自独立随机生成 SSRC，仍有 2^-32 的理论冲突概率。这符合 RFC 3550 的随机 SSRC 模型，这次不为它增加检查。
- 在没有 `/dev/urandom` 和 `/dev/random` 的 chroot 环境中，ireader 仍会退回 `rand()`；当前的部署方式下不存在这种情况。
- librtmp 握手的随机数仍然由 `srand(time(NULL))` 生成，属于 ireader 自身的实现，没有改动。

## 6. 文件

- `port_pool_check.cpp`、`rtp_ssrc_check.cpp`：使用 `../20261104-library-review/build_check.sh` 编译；
- `jobs/*.sh`：测试机上运行的任务。`r02b` 重跑了因运行目录缺少 `.cache` 夹具而 BLOCKED 的轮次；
- 原始结果只保存在测试机上的 `runs10/`。
