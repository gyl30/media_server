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

## 验证记录

| 目录 | 版本 | 内容 | 缺失引用 |
| --- | --- | --- | --- |
| [20261023-04334df-final-acceptance](../verification_results/20261023-04334df-final-acceptance/README.md) | `04334df`→`3cbc170` | 核心最终验证与稳定性验收 | `harness/*.cpp` 丢失；`san_env.sh` 的 `LD_LIBRARY_PATH` 做法已被静态 sanitizer 运行时取代 |
| [20261023-3cbc170-rfc822-race](../verification_results/20261023-3cbc170-rfc822-race/README.md) | `3cbc170` | `gmtime` 数据竞争修复验证 | — |
| [20261023-dfe30e6-web-console](../verification_results/20261023-dfe30e6-web-console/results.md) | `dfe30e6`→`e12340d` | Web 控制台验收 | — |
| [20261023-e12340d-gb-camera](../verification_results/20261023-e12340d-gb-camera/results.md) | `e12340d` | GB 实机摄像机验收 | 摄像机原始记录只在本地 |

## AAC afterburner=0 的证据（2026-10-29 复核）

- 主观：只有一位使用者的非正式试听反馈“听不出区别”。`~/aac-listening/` 保留了 6 段素材及盲听答案，但没有记录作答结果，也没有 ABX，因此不能作为音质等价的证据，只能说明未发现明显劣化。
- 客观（项目子模块构建的 libfdk-aac，同 6 段 48 kHz 立体声 30 s，波形对齐后比较）：128 kbps 下关闭 afterburner 使 SNR 降低 0.8–7.9 dB（器乐段最大），全频带 LSD 变化 −0.02～+0.42 dB；64 kbps 下 SNR 降低 1.8–4.7 dB，LSD 变化 −0.18～+0.07 dB。码率不变，编码 CPU 为开启时的 42–56%。
- 延迟：开启与关闭的编码器 `initial_padding` 都是 2048 样本（48 kHz 下 42.7 ms），实测对齐偏移一致，afterburner 不影响延迟；服务端已按 `initial_padding` 前移时间线。
