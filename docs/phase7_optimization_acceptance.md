# Phase 7 优化接手审查与验收

2026-10-14。本次只接手 `perf/phase7-optimization`，不重建容量基线、不触碰 main、third 或其他 Agent 的资源。

## 版本与证据

- BASE_HEAD / origin/main 本地引用：`cafee7e28f96c3d8d998bb0191734a643edb9571`。
- START_HEAD：`b18d69ff268c788920ee6e0b9460730ee71685e3`。
- 已保留：`4abe523`（GB UDP packet 所有权移交）、`b18d69f`（HTTP-FLV completion 推进发送）。
- 新测试提交：`06fe0f0d4e30b998a75529cc077f3276e2c2aece`，仅增加 11 行关闭验证，不修改生产代码。
- FINAL_HEAD 为包含本文的审查提交，以 `git rev-parse perf/phase7-optimization` 核对，避免在提交中嵌入自身哈希。
- worktree：`/home/gyl/example/media_server_perf`；未 push、未 merge main、未改写历史。
- 原证据：DEV/TEST `/var/tmp/media_server_phase7_cafee7e_2ken9twc/`；完整逐臂 result 仍在 TEST。
- 新证据：DEV/TEST `/var/tmp/media_server_phase7_resume_b18d69f_sgMH9ot2/`；构建和 UBSan 回归在 DEV，GB 画像及探针在 TEST。
- 完整本地报告：`docs/performance_reports/phase7_optimization_review.md`，继续按既有 ignore 保留；本文仅提交安全摘要，不提交 token、raw log、媒体样本或 perf data。

## 两个优化的独立正确性审查

### GB UDP

调用链：`gb28181_rtp_sender::on_ps_frame()` → 锁定 ireader `rtp_ts_encode()` → packet callback → `gb28181_udp_sender_session::send_packet()` → `udp_transport::write(vector, endpoint)` → queue / completion。

- `media/gb28181/gb28181_rtp_sender.cpp:131` 将 packetizer 的可重用缓冲区复制成独立 vector；这次必要复制仍保留。
- `media/gb28181/gb28181_udp_sender_session.cpp:159` 删除的是 span 重载导致的第二次分配/复制，vector allocation 移入 datagram。
- `media/net/udp_transport.cpp:66` 保存完整包及 endpoint；`:87` 的 completion 同时持有 datagram，关闭清空 queue 后在途 buffer 仍有效。
- 移交前保存的 span 只在成功入队后的同 owner 同步段供 `rtp_onsend()` 读取；没有 yield，completion 不内联，锁定依赖不保存该指针。拒绝入队后立即返回，不再读 span。
- 内容、顺序、独立 SSRC/sequence、drop-newest 和错误由上层关闭的契约不变。没有增加状态或跨 worker 协调。

### HTTP-FLV

调用链：`on_frame()` → FLV muxer → writer callback → `enqueue()` → `start_write()` → completion 推进 queue。

- `media/http/http_flv_session.cpp:184` 保留 4 MiB 有界队列，空转非空启动写；`:205` 每次只有一项写入，completion 成功后 pop，再推进下一项。
- queue front 保有 body；Boost 1.92 Asio 按值保有 chunk sequence，Beast chunk_size 共享持有 header。局部 chunk 离开作用域不会使 header 悬挂。
- `safe_shutdown():300` **不提前清在途队列**；队列保留至 completion，成功逐项 pop，错误才 clear。不得为了立即回收而引入 buffer UAF。
- 接收协程和 write completion 有限保活；worker stop 的取消、逐层 shutdown 和 io 排空不变。FLV 初始化顺序、授权、claim、自然关键帧等待和 generation 不变。
- 消除的是反复创建发送 stackful coroutine / spawn task，仍有 chunk、payload、async_write 分配，不是无分配发送。

## 原始 A/B 复核

同 fixture、客户端 ELF、8 workers、CPU 0–7 / 8–15、1 源、group 200、warmup 25s、measurement 200s，三对顺序 AB/BA/AB；plain 轮次无 profiler。全部 viewer ready 且持续 progressing，无通过少发媒体获得收益的证据。

| 优化 | 三对 B−A CPU（核） | 平均降幅 | 结论 |
|---|---|---|---|
| GB / 1000 viewers | −0.045021 / −0.056440 / −0.033111 | 1.80% | 方向一致、主要来自 user CPU；n=3，不称稳健统计显著，不外推容量或固定收益。 |
| FLV / 800 viewers | −0.381916 / −0.340414 / −0.376142 | 19.24% | 收益明显且各对一致；峰值 PSS 均值 200.87→119.08 MiB，是分配/峰值改善，不是消除泄漏的证明。 |

GB 参数 95% t 区间约 `[−0.073835,−0.015880]` 核，FLV `[−0.422001,−0.310314]` 核；两者都依赖配对噪声的正态假设，三对同方向的双侧 exact sign p=0.25。媒体差均小于 0.012%。ELF SHA 而非 runtime 的 `result.head` 用于识别各臂；后者均记录工具包 BASE_HEAD。

## 补充验证

| 项目 | 状态 | 实际证据及边界 |
|---|---|---|
| RelWithDebInfo 完整构建 | PASS | `full-build.log`；实际 `-O2 -g -DNDEBUG -Werror`。 |
| 全量 CTest | PASS（隔离复验） | `full-ctest-isolated.log`，60/60，99.49s。原环境 59/60 和单项失败保留；Claude 服务占用 49154/55，未停止它。使用 `unshare -Urn` 独立 lo，未更改断言/端口池。 |
| FLV 关闭定向验证 | PASS | `flv-peer-close-ctest.log`，连续 3 次。source end / worker stop 后不先关 client，有限 deadline 等待服务端 EOF/reset，排除旧测试主动断开的混淆。 |
| UBSan 生命周期 | PASS | Debug 独立构建，`ubsan-targeted.log`，11/11、14.19s；UDP、TCP completion、GB sender、FLV、media lifecycle、跨 worker 分发。 |
| UBSan 实际混合媒体 | PASS | `ubsan-media/result.json`，GB UDP 输入，FLV3/GB UDP3/RTMP1/RTSP1/WHEP1/HLS1，15s 测量；全部媒体进展、owned 清理成功。属于功能验证，不是性能数据。 |
| UBSan 双轨解码 | PASS | `ubsan-decode-complete/`，上游及 GB 输入后的 FLV 各实际解码 60 帧、双轨 map、2s，stderr 空；FLV2/GB UDP2 后续 12s 健康。外部探针初次遗漏依赖的三份失败记录保留，不归因生产。 |
| ASan | BLOCKED | `asan-configure.log`，项目 Boost.Context ucontext/ASan 兼容守卫失败；未绕过、未将 UBSan 当 ASan。 |
| GB kernel profiling | PASS（有限画像） | 两臂 1000 viewers，150s；DUT DWARF30s、8核20s、stat10s，phase exit 全0、窗口内、媒体健康。raw 及 noinline self report 均保留。完整 inline DWARF report 两次60s超时，不能声称完整调用图已解析。 |
| 分配探针清理 | PASS（最小调用修正） | namespace 内 RED 可复现：tracefs 不可见，bpftrace exit0仍报 detach ENOENT。target 留在 namespace、tracer 在宿主按 PID attach 后 GREEN：exit0、stderr 空、events 前后相同、PID 消失。原警告不追认 PASS。 |
| 上轮负对照 / 实际解码 | PASS（证据复核） | 原 `negative-result.json` 五项及 `functional-result.json` 保留；没有重复整套 A/B 或声称本轮重跑负对照。 |

UBSan 插桩覆盖项目 C++ 和本构建的 ireader 目标；Boost、libSRTP、FFmpeg SDK、OpenSSL 等并非完整插桩依赖栈。不得宣称所有依赖完成内存安全验证。

GB FD 原失败是 DUT soft limit1024先耗尽：成功场景需2033 FD，各200-viewer客户端仅206。新启动父进程只设置自身 `ulimit -n 65536`，不修改全局或他人 limits。新两臂均2033 FD、1000 progressing、0 drop，全部 owned receiver 删除及进程回收。

新画像的 user CPU约0.6398→0.5929核，system约1.8850→1.8936核；worker约0.31–0.32核/线程，无明显单 worker 热点。两次10s原stat发送 `sendto` 2,138,445 / 2,150,883次，必须按墙钟约21.4万次/s理解，不能使用按 task-clock 归一的8.4万 `/sec`。这些是独立诊断，不混入原200s plain配对均值。

## 下一轮优化裁决与合并建议

- 新生产优化：0。sendmmsg batch2–4 模型收益不足，8–16 模型收益不等同生产收益，10-viewer queue 分布不能外推1000；不引入部分完成/重试框架。
- H.264 scanner 是真实每 viewer 热点，但共享表示或派生资源无法在本轮窄边界覆盖所有输入，延后。
- SRTP 多入口是必要 GCM IV/AAD/encrypt/final/tag，非五次重复加密；不动 third，不合并 viewer 密钥状态。
- 新版 FLV/RTMP 剩余分配未量化为可删除开销，不凭旧热点再改队列、buffer 或 executor。

**CONDITIONAL GO**：两个优化建议保留，未发现阻止保留的真实生产缺陷；正式合并前仍需在兼容 Boost.Context 的环境补 ASan 所有权/关闭回归，或由独立 Review 明确接受该验证限制。GB 收益应表述为已测三对的方向性小幅改善，而非稳健容量承诺。不自行合并。

本轮仅新增测试11行和审查文档；新增生产状态、配置、抽象、后台任务、自动重试、生产代码行数均0。本轮 namespace 无PID后已删除，owned探针事件为空，未改系统配置；原始失败及结果文件全部保留。
