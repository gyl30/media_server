# Phase 7 ASan 补充验收

2026-10-14。结论：**GO**，建议保留 Phase 7 两项生产优化，提交独立 Review 后可合并；本轮没有自行合并或推送。本报告只补充内存安全验证，不建立性能基线，也不改写历史 A/B 或首次失败。

## 1. 版本、范围与证据

- BASE_HEAD / 本地 origin/main：`cafee7e28f96c3d8d998bb0191734a643edb9571`。
- START_HEAD：`23e77ca000665247d78b6faba1c756f69c94e6fe`。
- 分支 / 唯一修改工作区：`perf/phase7-optimization`，`/home/gyl/example/media_server_perf`。
- 本轮代码验证版本：`66779b9c42dd69d723e687b578472f9116d63d04`；FINAL_HEAD 为包含本文的后续文档提交，以 `git rev-parse perf/phase7-optimization` 核对，不在提交中嵌入自身哈希。
- 保留既有 `4abe523`、`b18d69f`、`06fe0f0`、`23e77ca`；不 amend/rebase，不修改 main、Web worktree、third 或生产依赖。
- 全部新原始证据在 DEV：`/var/tmp/media_server_phase7_asan_23e77ca_zFCjAKc8/`。下文相对证据路径均基于该目录。
- 原完整报告仍在 `docs/performance_reports/phase7_optimization_review.md`，SHA256 保持 `0050fe41891106ee5b62b7ecf1a22ec3a04cad755fafcfbf88c01bf6a73123f6`；另有本轮只读副本 `original/phase7_optimization_review.md`。未改动原报告的第 1–9 节。
- 原 resume 和第一轮性能证据目录未清理。本次只在 DEV 做小规模功能验证；TEST 性能任务、namespace 和进程均未操作。

## 2. 原始 CMake BLOCKED 的确切根因

原 `asan-configure.log`、`CMakeCache.txt`、`CMakeConfigureLog.yaml` 已复制至 `original/`，不是依据一行 CMake 摘要推测。

`original/CMakeConfigureLog.yaml:1484–1495` 表明编译成功、链接失败：探测使用 `/usr/bin/g++-16`、`BOOST_USE_UCONTEXT`、`BOOST_USE_ASAN`、`-fsanitize=address,undefined`，实际链接 `/usr/local/lib/libboost_context.a`，第一个根本错误是：

```text
undefined reference to boost::context::detail::fiber_activation_record::current()
```

旧头文件和 CMake package 均为 Boost 1.92.0，没有发现 1.89/1.92 路径混用。旧 Context archive 含 `make/jump/ontop_x86_64_sysv_elf_gas.o`、`fcontext.o`、`stack_traits.o`，而没有 ucontext 的 `fiber_activation_record::current()` 定义。即 **ucontext 头文件路径与普通 fcontext 静态库实现不匹配**。

本次失败不是兼容守卫误报，未发现本次探测实现问题；该守卫只验证编译/链接兼容性，运行时安全另由后续测试验证。没有修改守卫、强写缓存或禁用 sanitizer。解决方法是隔离构建正确的 Context 和同版本组件，并让原守卫真实通过。

## 3. 隔离 Boost 与实际构建环境

用户后续指定优先采用本地 Boost 1.92.0，覆盖最初的 1.89 要求。

| 项目 | 实际值 |
|---|---|
| DEV | Ubuntu 22.04.5，内核 `6.8.0-136-generic` |
| Boost source | `/home/gyl/Downloads/boost_1_92_0`，version.hpp 为 `109200` / `1_92` |
| Boost prefix | 证据目录下 `boost-1.92-asan/`，不覆盖 `/usr/local` |
| 编译器 | `/usr/bin/gcc-16`、`/usr/bin/g++-16`；16.0.1 experimental，`20260315 r16-8100-g3aca3bae8ee`，不是 16.2 |
| Boost.Build | `toolset=gcc-16 variant=debug link=static runtime-link=shared threading=multi context-impl=ucontext` |
| 编译选项 | `define=BOOST_USE_UCONTEXT define=BOOST_USE_ASAN`，`-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer` |
| 组件 | Context、JSON、Program_options；JSON 自动引入静态 Container，均来自同一个 prefix |
| 项目构建 | 独立 `build-asan/`，Debug，`MEDIA_SERVER_SANITIZER=address`，测试和 benchmarks 开启 |
| FFmpeg SDK / CLI | `/home/gyl/ffmpeg901`；CLI `/home/gyl/bin/ffmpeg`，`n7.1.1-57-g1b48158a23-20250824`，外部 CLI 本身不是本轮 ASan 构建 |

完整命令保留在 `build-boost-1.92.sh`、`user-config.jam`、`configure-project.sh`，实际编译日志为 `logs/boost-1.92-build.log`；`:179` 包含 `fiber.cpp` 的宏及 sanitizer 编译选项。新 Context 的符号检查有 `fiber_activation_record::current()` 和 ASan hook 引用。

配置明确使用 `Boost_DIR=.../boost-1.92-asan/lib/cmake/Boost-1.92.0` 和同一 `CMAKE_PREFIX_PATH`。Imported targets 的 include 为该 prefix 的 `include`，archive 分别为其 `lib/libboost_{context,json,program_options,container}.a`。没有使用普通 `/usr/local/lib/libboost_context.a`。

`logs/project-configure.log:117` 的 `MEDIA_SERVER_BOOST_CONTEXT_ASAN_COMPATIBLE` 为 Success；新增测试后的重新配置也成功。CMake 自己执行检测，没有预设 TRUE。实际 `media_core.dir/flags.make:5,7,9` 和 link.txt 保留了正确的 prefix、两个宏、`-fsanitize=address,undefined`、`-fno-sanitize-recover=all`、`-fno-omit-frame-pointer`、`-Werror`，不只是一个 CMake 变量。

### 运行时身份

宿主动态库搜索默认会选 `/usr/local/gcc-16.2/lib64` 的 sanitizer。初次测试证据如实保留；最终验证用本轮 `sanitizer-runtime/` 中的 symlink，**仅通过子进程环境**锁定 GCC 16.0.1 包的 libasan8/libubsan1，无系统库替换：

| 文件 | SHA256 |
|---|---|
| `build-asan/media_server.asan-elf` | `7c19200494e8d6a00b2e1bbe0a095cb5161715088fb88f153cdc700efc8116b9` |
| `build-asan/media_server`（诊断 exec 包装器，不是 ELF） | `9a7ccac589af4be82c69fa7b2e20fbe2efcc07985dc02b4136ecfb95ad997b79` |
| `boost-1.92-asan/lib/libboost_context.a` | `c7b550eb37a3cfe4e83697a41646e9c220755328a5af55919b54ce5213559af4` |
| `/usr/lib/x86_64-linux-gnu/libasan.so.8.0.0` | `75440a83c5c004048324c1120f5137a275520b27baa84706daf00601180106c4` |
| `/usr/lib/x86_64-linux-gnu/libubsan.so.1.0.0` | `655840b8ea4f3901281a86de862322629ea5b7e543e2af458d1af25c5e34c364` |

compiler、libasan8、libubsan1 的包版本均为 `16-20260315-1ubuntu1~22~ppa1`。`logs/elf-matched-runtime.ldd.txt` 和 `smoke/ldd.txt` 记录实际选择；普通 libstdc++/libgcc_s 仍由宿主的 `/usr/local/gcc-16.2/lib64` 解析，不能称整台主机工具链全部来自同一安装。

## 4. 真实 fiber 切换及 ASan/LSan 正对照

`fiber-smoke.cpp`、`sanitizer-controls.cpp`、`run-smoke.sh` 均在仓库外。正常 smoke 编译时检查 `__SANITIZE_ADDRESS__`，使用项目相同 Context 宏和两个 sanitizer。

- 最终匹配运行时下连续 5 次 exit 0；每次 200 个 fiber、3,200 次 yield，进入独立协程栈、反复 resume/yield、结束并销毁，栈数组和堆对象跨切换保持正确。
- 故意 heap-use-after-free：exit 1，确实检出 `ERROR: AddressSanitizer: heap-use-after-free`。
- 故意泄漏：exit 1，LSan 确实检出 512 字节、1 个 allocation。
- `detect_leaks=1`、`halt_on_error=1`、`detect_stack_use_after_return=1`；没有 suppression、关闭报告或忽略测试非零退出码。
- 证据：`smoke/fiber-{1..5}.{stdout,stderr,exit}`、`smoke/{uaf,leak}-control.*`、`logs/smoke-matched-runtime.*`。初次默认运行时结果另保留于 `smoke-initial-runtime/`。

正常 stderr 存在 makecontext/swapcontext 的 sanitizer 兼容警告，没有隐藏或视作内存错误。它仍是验证边界，不宣称 sanitizer 对任意第三方协程切栈完全无局限。

## 5. EMFILE 测试环境故障及最小修正

首次全量 59/60，165.06 秒，exit 8，唯一失败为 `accept_recovery`。原 fixture 在 soft/hard limit 64 下确认真实 EMFILE，然后关闭 held peers。服务不是收到 SIGTERM，也不是生产代码自动退出：

`logs/accept-trace.36615:3090–3101`：accept → EMFILE → 原错误日志 → held socket EOF → sanitizer 内部 `pipe2` → EMFILE → `SanitizerTool: CHECK failed` → `exit_group(0)`。

这是 sanitizer 自身的内存可读性检查需要两个 FD，但业务 socket 尚未完成关闭造成的测试环境障碍。独立匹配 runtime 后仍复现，不能归因于默认 loader 路径。UBSan vptr 的可读性检查调用链可核对 [GCC r16-8100 源码](https://github.com/gcc-mirror/gcc/blob/3aca3bae8ee373b50d342f1e9ebbcb03f7d8e83a/libsanitizer/ubsan/ubsan_type_hash_itanium.cpp#L193)。

提交 `309940a`：`tests/accept_recovery.py:21,81–85` 初始 soft=64/hard=66；**先确认原 EMFILE 日志**，再 `resource.prlimit(server.pid, ..., (66,66))`，然后断开 held peers、执行原恢复断言。只改变自有子进程的解除压力条件，不改系统 limits、生产 listener、重试间隔或 RTSP 200 要求。prlimit 失败不吞掉。

- GREEN：同 ASan fixture 连续 3 次通过，合计 10.89 秒，实际输出 53 held connections、64→66、恢复 OPTIONS 200。
- 原 hard=64 fixture 对普通生产二进制也实际通过：`logs/accept-original-normal-complete.log`。初次外部复制脚本遗漏 Python import path 的失败保留为 `accept-original-normal.log`，不归因生产。
- 边界：ASan GREEN 不证明“恢复时仍保持 soft=64、仅靠 peer 关闭回收 FD”；它证明“soft64 下真实 EMFILE，明确解除压力后恢复”。原测试只有五秒期限内恢复断言，没有数值上至少三秒的断言；生产三秒 retry 保持不变。

## 6. LSan 发现的测试客户端引用环

首份真实混合运行 `asan-mixed/` 媒体健康，但 WHEP native client 退出时 LSan 报 106,452 字节、507 项 allocation，退出 1；不能把它记成 PASS。DUT 和 upstream 均 exit 0。

准确持有链：`play_session::peer_` → peer 的 packet/error callback → lambda 捕获 shared_ptr<play_session> → peer。`bench/whep_fanout.cpp:237–244,353` 与原 `bench/clients/webrtc_client.cpp:436–437,659–664` 相互持有，原 close 没有清回调。LSan 分配栈包含 peer、play_session 和两个 callable，不是仅有 OpenSSL 全局缓存，也不是本次两项生产优化引入。

提交 `66779b9`：

- `bench/clients/webrtc_client.cpp:665–666` 关闭时清空两个 callback；`:365,377,417` 调用前保留局部副本，防止回调内 close 销毁正在执行的 callable。
- `tests/webrtc_client_lifecycle.cpp` 与 benchmark-enabled CTest 注册：50 次立即/重复关闭，排空真实 cancellation，弱引用观察 parent/peer/context 释放，原 UDP 端口再绑定；第 51 次既有十秒 keepalive 向未配置端点产生真实发送错误，回调内两次 close。
- 稳定 RED：旧代码 iteration 0 对象不释放，exit 8，LSan 报 24,284 字节、331 项 allocation（`logs/client-lifecycle-red.log`）。
- GREEN：1/1、10.03 秒；完整 CTest 和修复后的实际混合媒体也通过。
- 0 新状态、0 新接口、0 新 executor、0 新锁；不修改 `media/**`、`signaling/**`、`third/**`。

局部 callable 副本可能增加测试客户端的每包成本，不是服务端热路径优化。未来容量比较必须固定 client ELF；本轮 ASan 数据不能混入历史 plain A/B。

## 7. 实际构建与测试矩阵

| 项目 | 最终状态 | 实际结果 / 证据 |
|---|---|---|
| CMake ASan configure | PASS | 原兼容守卫真实 Success，`logs/project-configure.log`、`client-red-configure.log`；未改 guard |
| Boost.Context fiber smoke | PASS | 最终 runtime 5 次，200 fibers / 3,200 yields 每次，exit0 |
| 完整 ASan 构建 | PASS | `logs/project-build.*`、`project-final-build.*`，exit0；包括所有已启用测试及 benchmark |
| ASan directed tests | PASS | 最终 12/12、13.15s，`logs/directed-ctest-final.*`；初次 11/11、8.86s 的证据保留 |
| ASan full CTest | PASS | 最终 61/61、152.09s、exit0，`logs/full-ctest-final.*` 和 `full-final-LastTest.log`；首次 59/60 不删除 |
| GB UDP lifecycle | PASS | sender 5 个具名场景；UDP 顺序、overflow、发送错误、在途关闭、跨 worker、RTCP accounting、端口归还和 weak expiry；另有真实 SR/RR |
| HTTP-FLV lifecycle | PASS | 11 个具名场景，包含 slow_close、pending_end、pending_stop、queue_overflow、writer failure、源替换、正常音频 chunk；真实双轨覆盖见后述 FFmpeg 探针 |
| Mixed media | PASS | 修复后 10 viewers、6 种输出、15s，全部 ready/progressing、客户端 exit0 |
| FFmpeg decode | PASS | 独立上游及 GB 输入后 FLV 各 60 帧 / 2s、双轨 map、stderr 空；RTCP 链路再解码 90 帧 / 3s |
| Source end / shutdown | PASS | 定向 source/end 与 worker stop 排空，真实媒体自有 receiver/sender 删除、DUT exit0、owned PID 全部消失 |
| LeakSanitizer | PASS | detect_leaks=1，512B 正对照检出；测试客户端泄漏 RED→GREEN；最终实际运行无未解决 LSan 报告 |

定向 12 项是 UDP、6 项 TCP transport、GB sender、HTTP-FLV、media lifecycle、worker sink fanout、WebRTC 测试客户端 lifecycle。完整 CTest 还实际覆盖 SDP、授权/一次性消费、ICE/DTLS/SRTP、WHEP DTLS 重传耗尽/迟到完成、RTSP pull 关闭、RTMP 迟到媒体、FFmpeg H264/H265 解码及 TCP/UDP stream auth。没有排除失败测试或修改健康阈值。

### 测试隔离与诊断门禁

使用 `unshare -Urn` 独立 user/net namespace，仅启用其 lo，不占 Pi namespace，不停止任何其他服务。完整与定向 CTest 均 -j2，遵循既有 RESOURCE_LOCK。没有全局 sysctl、limits、网络或 allocator 修改。

某些 Python 测试隐藏 stderr，且清理 helper 不检查 server 最终 exit code。因此只在本轮 build 产物处保留真实 `media_server.asan-elf`，以 exec 包装器预开 stderr；遇故障注入 preload 时把 libasan 放在其前面，stdout、PID、参数和信号不变。没有改测试断言或生产入口。

最终独立扫描 `logs/final-sanitizer-scan.json`：79 个实际日志文件，0 fatal diagnostic；包括 stderr、ASan/LSan/UBSan、`SanitizerTool: CHECK failed` 和 runtime-order 失败。56 条 makecontext 警告是扫描文件中的出现次数，不是 56 个内存错误。故意错误正对照和历史 RED 单独保留、明确排除于 GREEN 门禁，而非删除。

最初 full 的 stderr 位于 `server-diagnostics-initial/`；后续 `server-diagnostics/` 为最终实际测试，移动记录不改变原日志内容。双轨 decode 的旧 PID stderr 单独纳入最终扫描。

## 8. 小规模真实媒体证据

固定 fixture：`/var/tmp/media_server_phase7_cafee7e_2ken9twc/assets/fixture.flv`，SHA256 `3e100535299f428ee13dfde6ab789aaa1f326129932cbb4a4163a97c585761e8`，实测 H.264 Constrained Baseline Level 3.1、1280×720、30fps，AAC-LC 48kHz stereo；不是摄像机 Main 5.1。

1. `asan-decode/result.json`：RTMP 上游 → GB UDP sender → GB UDP receiver → FLV。上游和下游各真实双轨解码 60 帧、2 秒；后续 FLV2、GB UDP2 运行 12 秒，4/4 healthy。两个 ASan 服务 exit0，owned receiver 清空，进程全部回收。
2. `asan-mixed-complete/result.json`：相同 GB 输入，FLV3 / GB UDP3 / RTMP1 / RTSP1 / WHEP1 / HLS1，3 秒 warmup、15 秒测量；10/10 ready 且 progressing。FLV 1,347 video / 2,112 audio messages；RTMP 449 / 704；RTSP 2,589 / 705；WHEP 2,587 video / 751 audio packets，0 unprotect failures。queue_full / no_buffer_space / udp_drops 均 0。DUT/upstream exit0，全部 owned receiver 和进程回收。
3. `asan-rtcp/result.json`：单自有 DUT，合法一次性 publish/play 和 control 授权；RTMP → GB sender → 相邻 UDP 代理 → GB receiver → FLV。33 秒，6,959 RTP 包，七个进展窗都非零，1 SR、9 RR；首次 SR 26.5055 秒，实际跨越既有 25 秒 sender timer。双轨实际解码 90 帧、3 秒。DUT exit0、decoder exit0，清理错误为空，正常 terminate 的外部 FFmpeg publisher exit255。

native WHEP 是真实 ICE/DTLS/SRTP 收包及音视频进展，不是浏览器画面解码；本轮没有重跑 Chrome。RTCP 统计来自实际转发的数据包，不是只调用 rtp_onsend()。第一、二项 runner 的外部 FFmpeg publisher 最终为 −9（既有有界清理的 fallback），没有把它记为优雅 exit0；该 CLI 不属于项目 ASan 插桩对象。

这些全部为 DEV 隔离 loopback 功能测试，不是 TEST 容量/真实性能结果；没有跑 800/1000 viewer 或重跑 A/B。

## 9. 复现、插桩和清理边界

在 DEV 中只读核验已经保留的独立构建与 prefix：

```bash
evidence=/var/tmp/media_server_phase7_asan_23e77ca_zFCjAKc8
sha256sum "$evidence/build-asan/media_server.asan-elf" \
  "$evidence/boost-1.92-asan/lib/libboost_context.a"
sed -n '1,120p' "$evidence/run-smoke.sh"
sed -n '1,120p' "$evidence/run-directed.sh"
sed -n '1,120p' "$evidence/run-full-ctest.sh"
```

这些脚本是原始执行记录，不是可在原目录重复执行的入口。`run-smoke.sh` 内部硬编码 ROOT，会覆盖 smoke 产物及日志；CTest 会更新 build 中的 LastTest.log，诊断包装器也有固定输出目录。复跑必须先复制脚本至新证据目录，调整所有内部 ROOT、独立 build/prefix、包装器诊断输出和媒体 runner output，并设置新目录的 sanitizer runtime 环境；仅重定向外部 stdout/stderr 不足以防止覆盖。媒体 runner 的自有结果目录默认拒绝覆盖。原始脚本、config/compile/link 信息、ELF、静态库、fixture SHA、每项退出码和最终 `SHA256SUMS` 保留供审查。原始日志和 JSON 含敏感 token/URL，仅在受限仓库外存放，不提交。

- 第一方 C++、本构建的 ireader C 目标、独立 Boost 组件确有 sanitizer 编译选项。FFmpeg SDK/CLI、OpenSSL、libSRTP、Ada、spdlog/fmt 等不是本轮完整插桩依赖栈；ASan 拦截其分配并不等于检查其所有内部访存。
- ASan 对 ucontext 的兼容警告仍存在；5 次实际切栈及对应生命周期通过，但不能声称验证了任意平台/协程组合。
- 新离线 client test 没有 SRTP 或 packet callback 内关闭故障注入。真实 mixed 覆盖正常 SRTP/packet 接收，局部副本的重入保护还通过真实 error callback 内关闭验证。成功 send completion 关闭后可能有限保活到既有十秒 keepalive，不是本次永久环，未扩大修改。
- 保留 `media_server.asan-elf`、新 Boost 1.92 prefix、完整测试产物、全部日志、正对照、首次失败和复现命令。仅清理本轮已失去用途的 1.89 安装及 b2 中间产物（约 364MiB），原始构建日志及其库 SHA 保留；需要时可由本地源码及保留命令重建。
- 原 fixture、普通 `/usr/local` Boost archive/version.hpp SHA 未变；本轮自有媒体 PID 最终不存在。没有碰其他 Agent 进程或 TEST 资源。

## 10. 最终自审与合并建议

本轮没有修改两项生产优化，没有新增生产状态、抽象、配置、后台任务、重试、锁或生命周期协调。修复限于一个测试环境条件和一个已有测试客户端的真实引用环；新增测试随对应工具修复提交。

没有未解决的 ASan/LSan 内存安全报告。原阻塞已通过独立 Boost 1.92、真实守卫、真实 fiber、完整 CTest、实际双轨媒体和关闭路径解除。**GO**：建议用户在外部独立 Review 后合并 Phase 7 已验证提交；本代理不执行合并或推送。

GB 的 1.80% 仍仅是原三对方向性小幅改善，不外推统计显著性或容量；HTTP-FLV 的 19.24% 和峰值 PSS 改善仍按原 A/B 口径，ASan 补测不更改这些数字。后续客户端 ELF 已因正确性修复改变，不能直接混同历史负载发生器成本。
