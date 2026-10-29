# CHECKPOINT 5：阶段 5 验收

运行：`20261012T043240Z-76355e4` · TEST `<TEST_HOST>` · 命名空间 `msperf-20261012T043240Z-76355e4`
被测：`media_server` HEAD `76355e435a766ed6d6c58be5dc315d14256b4e65`，产物 SHA256 `e8f8e298…`，`dirty=false`
明细：`phase5_capacity_plan.md`（计划与预检）、`phase5_capacity.md`（结果与分析）、`phase5_tables.md`（逐档原始表）

## 结论

阶段 5 目标达成，**87 次运行、57 个唯一档位、711 个客户端组；78 次 PASS，0 次 DUT 失败、0 次资源预算触发、0 次中止**。

1. **未测到 DUT 容量边界。** 6 个输出协议（WHEP、HTTP-FLV、RTMP、GB UDP、GB TCP active、HLS）在单源 **1500 viewer** 下全部媒体完整，最高 DUT CPU 为 WHEP **5.00 核**（授权 8 核的 63%），未触及 6 核停止线。最高已验证健康负载：WHEP 1500（S=1）/ 1200（S=8）、HTTP-FLV 1500、RTMP 1500、GB TCP active 1500、GB UDP 1500（DUT 侧健康，工具因编排器判 INCONCLUSIVE）、HLS 1500、GB TCP passive 700（未继续加压）。
2. **实际停止原因全部不是 DUT 容量**：① 1500 viewer 测试上限（全部协议）；② `perf_run` 编排线程自身饱和（25 个客户端子进程时 0.93 核）；③ 调整前的 FD 软限制 1024。三者均已按分类记录，未计入 DUT 容量。
3. **4 核预警穿越点**：WHEP S=1 约 1061 viewer、WHEP S=8 约 918 viewer。其余协议 1500 viewer 时最高 3.92 核。
4. **真实约束比进程 CPU 更早**：WHEP 1500 viewer 时承载 DUT 的 8 核聚合繁忙率 **89.3%**（最大单核 92.0%），而进程 CPU 仅 5.01 核；差值为 loopback 发送 softirq（约 1.07 核当量）与未绑核的编排线程。**以进程 CPU 定义的 4/6 核预算会低估机器负载**，阶段 6 必须统一三口径。
5. **阶段 4 模型不可外推到千级 viewer**：WHEP 偏差从 +5.8%（200）单调恶化到 **−15.2%**（1500）；每 viewer 边际成本随负载下降（4.25 → 1.1 mCPU/viewer），原因是负载转入不计入进程 CPU 的内核路径，而非丢流（每 viewer 速率与 1-viewer 对照一致）。

## 前置校正与预检（已接受）

1. **阶段 4 统计口径**：单位（µCPU → mCPU）、最大残差百分比的分母（改为相对该点观测值，0.81%）、模型适用范围、5.99 mCPU/source 仅限 RTMP 输入路径。四处文档已修正，未重跑阶段 3/4。
2. **客户端能力预检**：确认所有负载生成器进程内单线程；同场量化 `--group-size` 拆分对 DUT 侧的影响（WHEP −0.9%、GB UDP +1.3%、FLV 300 viewer +1.0%），判定可用于容量测试；确认 GB 客户端单事件循环在 80 viewer 即达 0.525 核，据此设定各协议 group-size。
3. **FD 软限制 1024 → 65536**（用户批准，仅测试进程树）：运行中读取 DUT `/proc/<pid>/limits` 验证为 65536；外层 shell 仍 1024，未改全局 sysctl/PAM/systemd。效果：GB UDP 从最高 440 viewer（FD 913）提升到 1500（FD 3033），其余协议从 800 提升到 1500（FD 1533）。

## 无损性判据的修正（本轮方法学结论）

每 viewer 收包速率**不能**作为无损判据：fixture 视频字节速率随内容相位波动 ±4.5%（包速率恒定 30.0 pkt/s，波动 0.1%），发布端循环 12 s 素材使每个测量窗口落在不同相位。

**1-viewer 对照（3 次，与 1000-viewer 档交错）**给出决定性证据：单 viewer 为 207.8 KB/s、175.6 视频 pkt/s，与 1000-viewer 档相同且低于 200-viewer 档。单 viewer 不受扇出影响，故该波动与负载无关。全部 28 个 WHEP 档位音频恒为 50.0 pkt/s；实测每 viewer 速率区间 204.4–221.8 KB/s 落在 fixture 预测区间 204.8–222.9 KB/s 内。

采用的无损性证据链：逐 viewer ready/progressing、coverage ≥0.99、`unprotect_failures`/`runtime_failures`/`non_progressing_viewers`/`failed` 全程为 0（711 个客户端组）、逐 viewer 收包量下界与同相位 1-viewer 对照一致、服务端 `queue_full`/`no_buffer_space`/`udp_drops` 增量为 0。本轮客户端类型不上报 RTP 序号间隙计数器，序号间隙不在证据内。

## 执行量

| 批次 | 内容 | 运行数 / 墙钟 |
| --- | --- | --- |
| `phase5cal` | 客户端切分口径校准（3 协议 × 1/4 进程） | 6 例 / 4.8 min |
| `phase5ab` | 会话锚点 + WHEP S=1 200→800 + S=8 200→800 + 逐 source 取证 | 9 例 / 8.6 min |
| `phase5c` | GB UDP 160→700、GB TCP passive 160→700（首批 4 例 GB TCP 因输出名误写作废） | 12 例 / 7.5 min |
| `phase5d` | GB TCP active 160→700 补跑 + RTMP/FLV/HLS 200→800 + FLV 单/多进程桥接 | 16 例 / 14.8 min |
| `phase5e` | WHEP S=1 1000→1500、S=8 1000/1200、GB 1000、RTMP 400 复跑 | 11 例 / 13.0 min |
| `phase5f` | 最高档正式重复 ×3（7 协议）+ 1-viewer 对照 ×3 + HLS S=8 内存 | 22 例 / 23.5 min |
| `phase5g` | 每核 softirq 归因诊断（修正采样器字段索引后复测 800/1500） | 2 例 / 2.4 min |
| `phase5h` | 其余协议补压至 1500（FLV ×3、GB UDP ×3、RTMP、HLS、GB TCP active） | 9 例 / 11.5 min |

单例最长墙钟 79.3 s（WHEP 1500）；合计 **86 min** 机器时间。配置与阶段 3/4 完全一致：同 fixture、`--workers 8`、DUT 绑核 0-7、客户端绑核 8-15、warmup 10 s、测量 30 s、`min-free-memory 8192 / min-free-disk 2048 / max-server-pss 16384`。

## 隔离与安全预算

| 约束 | 要求 | 实测 |
| --- | --- | --- |
| DUT CPU | 4 核预警 / 6 核停止 | 峰值 5.01 核（WHEP 1500），6 核停止未触发；4 核预警在 WHEP 两档穿越并缩小步长至 100 |
| 可用内存 | ≥8192 MiB | 最低 79.9 GiB |
| 可用磁盘 | ≥2048 MiB | 未触发 |
| DUT PSS | ≤16384 MiB | 最大 455 MiB |
| 客户端 | 每进程 0.9 核 / 单线程 0.9 核 | 客户端进程最热线程最大 0.62 核（FLV 单进程 300 viewer）；4 次判定为编排线程自身，非负载生成器 |
| publisher | 存活 | 全程存活，单例最大 0.091 核 |
| 网络 | 无队列溢出/丢包 | `queue_full`/`no_buffer_space`/`udp_drops` 增量全程为 0；`unprotect_failures`/`runtime_failures`/`non_progressing_viewers` 全程为 0 |
| 全局配置 | 不修改 | 仅测试进程树 FD 软限制 65536（已批准）；sysctl、CPU 拓扑、命名空间未变 |

## 工具与负载生成器缺陷（未改代码，已记录）

1. **bench C++ 客户端测量终点竞态**：`bench/media_fanout.h:296` 仅在 io_context 定时器回调写 `measurement_end_monotonic_ns`，进程在 deadline 同刻退出时该值保持 0 → `perf_run` 判 coverage=0 → FAIL。711 组中命中 1 组（0.14%），同档复跑 PASS 且 DUT 数值一致（0.7649 vs 0.7621，差 0.4%）。
2. **`perf_run` 编排线程是子进程数上限**：160 子进程 → 1.00 核；25 个 GB 子进程 → 0.93 核。经验边界为客户端子进程 ≤25。
3. **GB 客户端单事件循环**：80 viewer 即 0.525 核；拆分后总 CPU 在 440 viewer 以上趋于 ≈2.8 核而与 viewer 数无关，说明成本主要来自事件循环。
4. 我的执行器缺陷（已修）：`gb_tcp` 应为 `gb_tcp_active`（4 次 TEST_INFRA_FAILURE，已补跑）；`/proc/stat` softirq 字段索引取错（修正后复测 2 例）。

## 目标逐项核对

| 目标 | 结果 |
| --- | --- |
| 各协议按递增负载确定最高健康负载 | 完成：6 协议到 1500 上限；GB TCP passive 停在 700（原因：其客户端侧核占用与 TCP active 同量级，而本轮未继续加压，不是 DUT 限制） |
| 区分 DUT / 客户端 / 网络与工具限制 | 完成：DUT 无限制命中；客户端限制仅在 GB 1500 的客户端核 100% busy 处出现；工具限制 2 项（编排线程、bench 竞态） |
| 验证阶段 4 WHEP 增长模型在高负载是否成立 | 完成：V≤800 偏差 ≤6%，V>1000 系统性高估进程 CPU，1500 处 −15.2%，不可外推 |
| 为阶段 6 准备稳定 profile 负载 | 完成：给出 4 组档位（`phase5_capacity.md` §10），均 ≥3 次重复、极差 ≤0.5% |
| 前置：阶段 4 统计校正与客户端预检 | 完成并被接受 |

## 基线漂移（本轮未引起，但影响阶段 6）

本轮全部 87 次运行均使用已批准的 `76355e4` 运行包（启动前校验 SHA256 `e8f8e298…`，`dirty=false`），基线未切换。但在本轮执行期间（2026-10-12 17:44 → 22:37）主仓库 HEAD 已从 `76355e4` 前进到 `1e3885e`，其中 6 个提交改动了生产代码，且正好集中在本轮测过的发送/错误路径上：

```
media/webrtc/whep_session.cpp            10 ±
media/webrtc/whip_session.cpp            10 ±
media/net/tcp_transport.cpp              10 ±
media/gb28181/gb28181_tcp_{receiver,sender}_session.cpp   12 ±
media/gb28181/gb28181_udp_sender_session.cpp   4 ±
media/rtsp/rtsp_publish_udp_session.cpp / rtsp_server_connection.cpp   11 ±
```

提交信息为“精简…错误回调 / 重复判空”。影响：

1. 阶段 5 结论仅适用于 `76355e4`，不得直接归到新 HEAD 上。
2. 阶段 6 开始前需先定基线：若用新 HEAD，WHEP / GB / TCP 的每 viewer 成本可能需要重新标定（这些改动位于发送与会话错误处理热路径）；若继续用 `76355e4`，则与阶段 1-5 完全可比。本轮未做任何假设，也未重新构建。

## 证据完整性事件（10-13 09:15）

我在部署阶段 6 时误触发了一次阶段 5D 批次脚本。`perf_run` 因输出目录已存在而直接拒绝启动（`FileExistsError`，1 s 退出），**未产生任何新测量**，阶段 5 的 `result.json` 与原始数据均未被覆盖；仅向 `phase5d/summary.jsonl` 追加了 14 条 `wall_seconds=1.0` 的重复行，已删除并备份为同目录 `summary.jsonl.duplicate-rows-removed`。阶段 5 报告数值不受影响。同时已给阶段 6 执行器加固：`runner_exit` 非 0 直接归为 `TEST_INFRA_FAILURE`，不再把旧 `result.json` 误读为 PASS。

## 未做与边界

- 未做 soak、未做突发接入/断开、未做跨协议混合负载（本轮全部为单协议单输入）。
- 未测到任何协议的真正容量边界；若需要，需要放宽 1500 viewer 上限并解决编排线程上限（可用多编排进程或提高每进程 viewer 数以减少子进程数）。
- 阶段 6 未开始；Off-CPU 仍受环境限制（`offcputime-bpfcc` 因 PyPI `bcc` 遮蔽与 `finish_task_switch.isra.0` 符号缺失不可用），`jeprof` 需从 DEV 打包。
- 生产代码未改，基线未切换，主仓库与其他 workspace 未受影响，未 push。
