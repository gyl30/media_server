# 历史报告导航

`docs/performance_reports/` 与 `docs/verification_results/` 下的报告都是当时版本、环境和工具的快照，结论不随后续代码自动更新，也不代表当前结果。当前做法和口径见 [performance.md](../performance.md)、[build-deploy.md](../build-deploy.md)；当前结果以对应源码重新测量为准。各目录只跟踪 `.md/.py/.sh/.patch/.diff/.c/.cpp/.h`，原始数据、二进制和夹具由运行者在本地保留，可能不存在。

## 性能报告

| 目录 | 被测版本 | 主题与结论 | 与当前代码的关系、缺失引用 |
| --- | --- | --- | --- |
| [20261011T151927Z-b4965b8-2737042](20261011T151927Z-b4965b8-2737042/README.md) | `b4965b8` | 双机流程预检，协议 smoke 8/8，之后未运行（PARTIAL） | 引用的 `bench/perf_smoke.py` 已不存在；`tests/` 与 ctest 在 `4b3654c` 删除，签名桩现为 `bench/signaling_stub.py` |
| [20261012T043240Z-76355e4-test](20261012T043240Z-76355e4-test/phase1_preflight.md) | `76355e4` | 阶段 1–5：预检、基线与容量规划 | 同上，早期工具链 |
| [20261013T011210Z-7788320-test](20261013T011210Z-7788320-test/phase6_baseline_calibration.md) | `7788320` | 新旧基线配对标定，A/B 无显著差异 | 早期工具链 |
| [20261023-891aec1-8core](20261023-891aec1-8core/report.md) | `891aec1` | 8 核全协议容量边界与瓶颈；列出 A1–A5、F1–F6 | A1、A2、A3、F1、F2 已在后续报告修复；`raw/`、部分 `tools/*.py` 与分析文本未保留 |
| [20261023-core-slimming](20261023-core-slimming/README.md) | `1ac4e04`→`04334df`、`3cbc170` | 核心精简的 A/B | 二进制和远端运行目录只在本地/测试机 |
| [20261024-a2-dispatch](20261024-a2-dispatch/report.md) | `c999003` 基线 | 跨 worker drain 公平性，采用 20 ms 时间片（ACCEPT） | 采用的实现为 `6d87f96`；B1/B2/B3 补丁与 `tests/dispatcher_test.cpp` 因扩展名未放行而从未跟踪，已随 2026-10-29 本地清理丢失，无法复现候选方案对比 |
| [20261027-a1a3](20261027-a1a3/report.md) | 见报告 | F1/F2 修复、TCP 背压 A1.1（ACCEPT）、GB TCP 合并写出 A1.2（REJECT）、UDP 接收缓冲 A3 | `tests/tcp_transport_test.cpp` 同上丢失；`28b8ede`、`3c6fddc` 等提交仍在历史中 |
| [20261027-whip-input](20261027-whip-input/report.md) | 见报告 | WHIP 输入瓶颈：worker 分布不均，8 核上限约 255–265 路 | 放置改动为 `83bf251` |
| [20261028-gb-udp-sender](20261028-gb-udp-sender/report.md) | 见报告 | GB UDP sender 常驻读取 RTCP socket | — |
| [20261028-whep-pull-profile](20261028-whep-pull-profile/report.md) | `cb03380` | WHEP Pull 分布审查，建议按会话数放置（OPTIMIZE） | 建议已在下一份报告实现 |
| [20261029-whep-pull-optimize](20261029-whep-pull-optimize/report.md) | `cb03380` 起 | 会话最少放置 `a636699`；FFmpeg pow43 补丁与 fdk 隔离实验 | `tools/ffmpeg/*.diff` 丢失；pow43 补丁等价内容可用 `git show 24c80ec^:deps/ffmpeg/patches/0001-aacenc-escape-pow43-table.patch` 取回；fdk 接入已进入主线 |
| [20261029-aac-whep-capacity](20261029-aac-whep-capacity/report.md) | `415e8aa`→`24c80ec` | fdk AAC 与 afterburner=0，WHEP Pull 496 路（仅作参考） | 报告中的 `deps/ffmpeg/build.sh`、`~/ffmpeg-ms` SDK 已由 `46d31ca` 的子模块构建取代；主观试听证据见下文 |
| [20261030-deps-regression](20261030-deps-regression/report.md) | `1d5cfc7` 起 | 依赖重组验收、全协议回归与性能哨兵；jemalloc 5.4.0 回归二分到 `a952a3b8`、`3c14707b`，固定 5.3.0 | 原始结果、二分用共享库只在测试机 |
| [20261030-mixed-workload](20261030-mixed-workload/report.md) | `a087a42`→`3b6c0bd` | 三种混合负载的 8 核容量：WebRTC 为主 1.2×→1.6×（Opus 直通），全协议广播约 +5%，多源少观看不变；修复 WHEP SR 音画偏差；租约统一放置 | 原始结果与 profile 只在测试机；B 数据在测试 netns 缩小临时端口范围后测得 |
| [20261030-protocol-matrix-performance](20261030-protocol-matrix-performance/report.md) | `3b6c0bd` 二进制（`92850b4` 生产源码相同） | 全协议矩阵 31/31；WHIP→各输出容量（每源 2 观看者：TCP/GB 480 源、HLS 544、WHEP 直通 384、Chrome offer 256、全输出 352）；Opus 直通只对带 `stereo=1` 的 offer 生效，Chrome 默认 offer 走 egress；按需 Opus→AAC 与按实际声道登记列为决策项 | 原始结果、profile、bpftrace 与浏览器数据只在测试机；上一轮 B 的直通收益只对 Firefox 类 offer 成立 |
| [20261031-ondemand-opus-aac](20261031-ondemand-opus-aac/report.md) | `8ce9f5c` 基线与实验构建 | 矩阵补 H.265→WHEP 与 GB 端到端解码（`4b09c66`）；按需 Opus→AAC：纯直通 WHEP 384→1056 源、无观看者 CPU −79%；Chrome 默认 offer 直通可行（停在评审） | 原始结果、bpftrace、浏览器数据与实验二进制只在测试机 |
| [20261031-gb-ps-rtsp-aac-chrome-opus](20261031-gb-ps-rtsp-aac-chrome-opus/report.md) | `b72e703` 基线与实验构建 | GB PS 自有解复用与完整音画验证（AAC/G.711A/G.711μ × 三种传输）、GB 测试客户端 ASan/TSan 无报告；RTSP AAC 约 10 ms 跳变定位到接收解包的整数毫秒递增；Chrome 默认 offer 直通为多目的地网关的 SHOULD NOT 偏离，cycles −67%、容量 256→1056 源（实验，未合入） | 原始结果、bpftrace、浏览器数据与实验二进制只在测试机 |
| [20261031-chrome-opus-passthrough](20261031-chrome-opus-passthrough/report.md) | `aa083a5` 加工作区改动 | GB PS 按完整 pack 核对尾部完整性与截断负向测试；矩阵对 RTSP AAC 锯齿严格 XFAIL/XPASS，30 PASS + 3 XFAIL；Chrome 默认 offer 直通转为生产改动，浏览器、ASan、8 核 1056 源回归通过；TSan 浏览器失败在 HEAD 同样复现 | 原始结果与 sanitizer 日志只在测试机 |
| [20261101-tsan-video-only-idle-transcoder](20261101-tsan-video-only-idle-transcoder/report.md) | `fc1bcc1` 与实验构建 | TSan 并发证书竞争与 SEGV 均为工具问题（OpenSSL 未插桩、协程栈上符号化溢出）；纯视频 WHEP 触发无人消费的 Opus→AAC，改挂伴随流使其 CPU −66–73%（候选，未合入）；空闲转码器每源约 0.37 MiB，不影响容量，不实施 | 原始结果只在测试机 |
| [20261103-full-acceptance](20261103-full-acceptance/report.md) | `0eae004` | 全协议验收与 8 核混合负载：WHEP 协商/生命周期、35 场景矩阵与 Sanitizer 通过；A/B/C 与 `3b6c0bd` 一致，纯视频观众混合 D 健康容量 1.8×→2.2×；profiling 无可归因优化，GO | 原始结果与 profile 只在测试机；混合负载 GB 音画探针与负载素材不兼容，报表另行区分 |
| [20261103-udp-io](20261103-udp-io/report.md) | `b131705` 与工作区候选 | UDP 收发归因；`sendmmsg`/connect REJECT，GSO 帧内合并 ACCEPT：GB UDP −46%、WHEP −12% cycles，D 2.2×→2.4×、B 1.6×→1.7×；avsync 素材消除混合负载 GB 探针缺口；遗留既有 WHEP SRTP replay_fail | GSO 已按维护性取舍撤销（见 [20261104 库优先审查](../reviews/20261104-library-first-review.md)），当前为逐包 `send_to`；原始结果、profile、bpftrace 只在测试机；收益在 lo 上测得 |

## 验证记录

| 目录 | 版本 | 内容 | 缺失引用 |
| --- | --- | --- | --- |
| [20261023-04334df-final-acceptance](../verification_results/20261023-04334df-final-acceptance/README.md) | `04334df`→`3cbc170` | 核心最终验证与稳定性验收 | `harness/*.cpp` 丢失；`san_env.sh` 的 `LD_LIBRARY_PATH` 做法已被静态 sanitizer 运行时取代 |
| [20261023-3cbc170-rfc822-race](../verification_results/20261023-3cbc170-rfc822-race/README.md) | `3cbc170` | `gmtime` 数据竞争修复验证 | — |
| [20261023-dfe30e6-web-console](../verification_results/20261023-dfe30e6-web-console/results.md) | `dfe30e6`→`e12340d` | Web 控制台验收 | — |
| [20261023-e12340d-gb-camera](../verification_results/20261023-e12340d-gb-camera/results.md) | `e12340d` | GB 实机摄像机验收 | 摄像机原始记录只在本地 |
| [20261104-library-review](../reviews/20261104-library-first-review.md) | `e26b4e0` 与工作区 | 库优先审查：撤销 UDP GSO、SRTP profile 改用 libsrtp；SRTP 三 profile 互通、UDP 发送专项、矩阵、Sanitizer、生命周期 | 原始结果只在测试机 `runs9/` |
| [20261104-port-pool-replay](../verification_results/20261104-port-pool-replay/report.md) | `93bb9eb` 与工作区 | 端口池范围改为编译期常量；WHEP SRTP replay_fail 根因：ireader 未定义 `OS_LINUX`，`rtp_ssrc()` 走全局 `rand()`，被 RTMP 握手的 `srand(time(NULL))` 重置，导致音视频 SSRC/seq 相同；确定性回归、矩阵、Sanitizer、生命周期通过 | 原始结果只在测试机 `runs10/` |
| [20261104-rtsp-aac-timeline](../verification_results/20261104-rtsp-aac-timeline/report.md) | `830668b` | RTSP AAC 多 AU 包时间戳锯齿的确定性回归（推流/Pull、48k/44.1k、回绕、丢包、跳变、SR）；ireader 不提供 AU 包内序号与帧长，候选方案均需第二套时间线或重复解析，KEEP，保留严格 XFAIL；另记丢包后额外丢弃完整 AU 与 960 样本帧风险 | 无生产改动；后续方向为 ireader 内最小修复 |
| [20261104-long-stability](../verification_results/20261104-long-stability/report.md) | `830668b` | 长稳验收：D 1.6×、B 1.2× 各 1800 s，WHEP/GB/RTSP/RTMP churn 各 600 s，全部 PASS HEALTHY，PSS/FD 无增长、清理后 FD 回到 32；WHEP 探针 80 ms 音频缺口确认为录制启动伪影；真实摄像机 BLOCKED（无配置授权）；NO CHANGE | 原始结果只在测试机 `runs11/` |

## AAC afterburner=0 的证据（2026-10-29 复核）

- 主观：只有一位使用者的非正式试听反馈“听不出区别”。`~/aac-listening/` 保留了 6 段素材及盲听答案，但没有记录作答结果，也没有 ABX，因此不能作为音质等价的证据，只能说明未发现明显劣化。
- 客观（项目子模块构建的 libfdk-aac，同 6 段 48 kHz 立体声 30 s，波形对齐后比较）：128 kbps 下关闭 afterburner 使 SNR 降低 0.8–7.9 dB（器乐段最大），全频带 LSD 变化 −0.02～+0.42 dB；64 kbps 下 SNR 降低 1.8–4.7 dB，LSD 变化 −0.18～+0.07 dB。码率不变，编码 CPU 为开启时的 42–56%。
- 延迟：开启与关闭的编码器 `initial_padding` 都是 2048 样本（48 kHz 下 42.7 ms），实测对齐偏移一致，afterburner 不影响延迟；服务端已按 `initial_padding` 前移时间线。
