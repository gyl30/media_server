# 流媒体核心最终验证与稳定性验收（2026-10-23）

- 基线 04334df，最终 3cbc170（修复长度前缀载荷遇 1 字节 NAL 时的误判）。
- 本目录被 `.gitignore` 忽略，保存所有脚本、夹具、日志和原始结果。性能基线归档见 `docs/performance_reports/20261023-core-slimming/`。

## 构建

| 目录 | 配置 | 说明 |
|---|---|---|
| `build/` | RelWithDebInfo，jemalloc | 生产配置 |
| `build-asan/` | Debug + `-D_GLIBCXX_ASSERTIONS`，`MEDIA_SERVER_SANITIZER=address`（即 `-fsanitize=address,undefined -fno-sanitize-recover=all`） | Boost.Context 用私有 ucontext 构建：`boost-ucontext-asan/`（`scripts/build_boost_context_ucontext.sh`，源码在 `boost-context-src/`，取自 boost-1.92.0 标签） |
| `build-ubsan/` | Debug + `-D_GLIBCXX_ASSERTIONS`，`MEDIA_SERVER_SANITIZER=undefined` | 标准 fcontext |
| `build-tsan/` | Debug + `-D_GLIBCXX_ASSERTIONS`，`MEDIA_SERVER_SANITIZER=thread` | 标准 fcontext |

- 运行时通过 `sanitizer-runtime/` 中的 gcc-16 包运行时（`16-20260315-1ubuntu1~22~ppa1`）加载，见 `scripts/san_env.sh`。二进制默认解析到的 `/usr/local/gcc-16.2/lib64` 版本与编译器不匹配。
- ASan 选项：`detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=1:check_initialization_order=1:strict_init_order=1`。TSan 选项：`halt_on_error=0:second_deadlock_stack=1`，有报告时退出码为 66。
- ASan + ucontext 每次都会写一条 `ASan doesn't fully support makecontext/swapcontext` 警告，所以 `report_files` 不为 0，但诊断数是 0。

## 对照（证明工具能检出问题）

`harness/sanitizer_controls.cpp` 在 50 个协程内触发问题，结果在 `controls/`：

- ASan+UBSan 构建：UAF、泄漏、有符号溢出都能检出（退出码 1）；clean 用例退出码 0。
- UBSan 构建：有符号溢出能检出。
- TSan 构建：数据竞争、UAF 都能检出（退出码 66）；clean 用例 0 个报告。

## 场景与脚本

| 场景 | 脚本 | 结果目录 |
|---|---|---|
| perf_run 矩阵：mixed 8 种输出、7 种输入、source_stop、3 种 token 故障 | `scripts/matrix.sh <kind> <bin> <tag>` → `run_perf.sh` | `runs/pf-<kind>-3cbc170-*`，日志 `logs/matrix-*.log` |
| 生命周期：稳定观看、60 次抖动连接、300 个畸形或半截连接、源被 SIGKILL、播放已结束的流、新 stream_id、带活动会话 SIGTERM | `scripts/run_lifecycle.sh` → `lifecycle.py` | `runs/lc-*` |
| Chrome WHEP 9 项 | `scripts/run_chrome.sh` → `chrome_whep.py`（Playwright 加载未修改的 `signaling/web/whep.js`） | `runs/chrome-*` |
| GB28181 对讲 | `scripts/run_pair.sh <kind> <bin> <run> talk` → `talk.py` | `runs/talk-*` |
| WHEP 级联拉流（B 用 RTMP 推流，A 用 WHEP 拉 B，再用 RTSP/FLV 解码 A） | `scripts/run_pair.sh … whep_pull` → `whep_pull.sh` | `runs/whep_pull-*` |
| 长度前缀载荷字节级对比 | `harness/lp_check.cpp`、`harness/build.sh` | `logs/lp_check-*.log` |

- 汇总工具：`scripts/table.py runs 'pf-*-3cbc170-*'`、`scripts/inspect_run.py`、`scripts/scan_reports.sh`。
- 被测服务端和客户端都在本机回环地址上：perf_run 用 127.0.0.1/127.0.0.2，Chrome 用 127.0.0.3。

## 无效或已替代的 run

- `chrome-asan-3cbc170-invalid-stale-server`：strace 包装的 release 服务端在 strace 收到 SIGTERM 后脱离，残留进程继续占用 127.0.0.3，导致 ASan 服务端绑定失败，响应来自残留进程。已改为 `pgrep -x media_server` 守卫并重跑，见 `chrome-asan-3cbc170`。
- `lc-tsan-3cbc170-aborted-script-race`：脚本自身的竞态，`process.kill()` 时进程已退出。脚本修正后重跑为 `lc-tsan-3cbc170-b`。
- `pf-ubsan-3cbc170-in-rtsp_tcp`：24/24 progressing，但 rtmp 组 `measurement_end_monotonic_ns=0`，覆盖率 0。原因是 `bench/media_fanout.h` 客户端的计时竞态，重跑见 `-r2`。
- `pf-asan-3cbc170-fault-source_stop`、`pf-tsan-3cbc170-in-gb_tcp_passive`：BLOCKED，perf_run 读取已退出的 `gb_sender_fanout.py` 的 `/proc/<pid>/fd` 被拒。服务端正常，重跑见 `-r2`。
- `chrome-release-strace`：只用来证明服务端会发送 DTLS close_notify（`.trace` 中 39 字节 `\25\376\375` 记录），不作为功能结果。
