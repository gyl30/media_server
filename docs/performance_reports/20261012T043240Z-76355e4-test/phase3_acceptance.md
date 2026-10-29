# CHECKPOINT 3：阶段 3 验收

运行：`20261012T043240Z-76355e4` · TEST `<TEST_HOST>` · 命名空间 `msperf-20261012T043240Z-76355e4`
被测：`media_server` HEAD `76355e435a766ed6d6c58be5dc315d14256b4e65`，产物 SHA256 `e8f8e298…`，`dirty=false`
详细数据：`phase3_baseline.md`（基线）、`phase3_protocol_comparison.md`（协议矩阵）

## 结论

阶段 3 目标全部达成，**99/99 正式用例 PASS，0 失败、0 中止、0 资源预算触发**。

- 建立了可复现的空闲 / 严格 publish-only / publish+1 viewer 三级基线，并给出 publisher 与 signaling 的独立成本。
- 完成 8 个输出协议 × {1,10,50,100} viewer × 3 次重复的对比矩阵，全部有真实媒体证据。
- 每个协议在 1→100 viewer 区间内服务端成本线性（R² ≥ 0.98），重复波动 < 2.1%（绝对 < 0.006 核）。
- 100 viewer 内没有任何饱和迹象：线程未饱和、UDP 无积压、TCP 重传 0、PSI 全程 0.00。

## 执行量

| 项目 | 内容 |
| --- | --- |
| 能力探测 | 8 协议 × 1 viewer，8/8 PASS（`phase3-probe/`，5 s + 20 s 窗口，仅用于确认可测性） |
| 正式矩阵 | 8 协议 × 4 档 × 3 次 = 96 用例（`phase3-protocols/`，10 s + 30 s 窗口） |
| A3 校验 | publish + 1 FLV 校验 viewer × 3 次（同一矩阵内，`b-none-1-r*`） |
| 基线采样 | 空闲 × 3、严格 publish-only × 3、publisher/signaling 拆分 × 3（`phase3-baseline/`） |
| 墙钟时间 | 矩阵 06:00:47Z → 07:17:41Z，76.9 min（46.6 s/用例）；基线与探测另计 |

矩阵配置与探测配置（warmup/duration 不同）在聚合中按 `window_config` 分开分组，未混合统计。

## 隔离与资源预算

| 约束 | 要求 | 实测 |
| --- | --- | --- |
| DUT CPU | 固定物理核 0-7，`--threads 8` | 全程 `taskset 0-7`；线程迁移最大 25 次/用例 |
| 客户端 CPU | 不相交核 8-15 | 全程 `taskset 8-15`；100 viewer 时客户端总占用最高 0.64 核 |
| SMT 兄弟核 16-31 | 排除 | 未参与任何绑定 |
| 网络 | 专用命名空间 | `msperf-…`（198.19.0.10/.11），TCP 重传 0、listen drop 0 |
| 最低可用内存 | 8 192 MiB | 全程最低 82 535 MiB |
| 最低可用磁盘 | 2 048 MiB | 全程最低 65 210 MiB |
| DUT PSS 上限 | 16 384 MiB | 最大 47.5 MiB（WHEP 100 viewer） |
| 系统 PSI | 触发即停 | cpu / memory / io 全程 0.00 |
| 全局 sysctl / ulimit / cgroup | 不修改 | 未修改 |

## 关键数据（详见两份文档）

服务端 CPU 中位数（核）：

| 协议 | 1 | 10 | 50 | 100 | 每 viewer 斜率 |
| --- | --- | --- | --- | --- | --- |
| HLS | 0.0073 | 0.0113 | 0.0287 | 0.0481 | 0.41 mCPU |
| RTMP | 0.0123 | 0.0540 | 0.1201 | 0.2049 | 1.85 mCPU |
| GB TCP active | 0.0130 | 0.0554 | 0.1254 | 0.2128 | 1.92 mCPU |
| GB TCP passive | 0.0130 | 0.0557 | 0.1261 | 0.2138 | 1.93 mCPU |
| FLV | 0.0140 | 0.0677 | 0.1508 | 0.2569 | 2.32 mCPU |
| RTSP | 0.0133 | 0.0604 | 0.1508 | 0.2633 | 2.43 mCPU |
| GB UDP | 0.0130 | 0.0577 | 0.1551 | 0.2853 | 2.66 mCPU |
| WHEP | 0.0634 | 0.1478 | 0.3068 | 0.4997 | 4.23 mCPU |

基线：空闲 0.0000 核 / 15.9 MiB PSS；严格 publish-only 0.0062 核 / 19.9 MiB / 33 FD / 10 线程（原报 0.0089 核为 1 Hz 采样估计量偏差，已在 `phase3_baseline.md` 修正）（已用 `ss` + 控制 API 双重确认 0 viewer）；publisher(ffmpeg) ≈0.01 核 / 26.6 MiB；signaling 0 核 / 13.8 MiB。

媒体健康：96 个正式用例全部 `requested == ready == progressing`、`failed = 0`、`unprotect_failures = 0`、覆盖率 ≥ 0.9997；每 viewer 稳态速率与 fixture 完全一致（FLV/RTMP 29.9 视频 + 46.9 AAC 每秒；RTSP 176 RTP + 46.9 音频；WHEP 176 RTP + 50 Opus；HLS 15 个 2 s 分片；GB 208 KB/s），且这些计数随 viewer 数严格线性。

## 可回答的问题与新增发现

1. 各协议的每 viewer 边际成本首次有了定量排序，HLS 最省（0.41 mCPU），WHEP 最贵（4.23 mCPU + 最高固定成本）。
2. GB28181 UDP 比 TCP 贵约 38%（2.66 vs 1.92 mCPU），TCP active 与 passive 无差异；UDP 路径每 viewer 额外 1 个 FD。
3. FLV 比同内容的 RTMP 贵 25%（2.32 vs 1.85 mCPU），差异在 HTTP 封装路径。
4. 100 viewer 内没有单线程瓶颈（最大单线程 0.105 核 / 8 线程），说明当前并发模型在此规模未成为限制。
5. WHEP 的线程上下文切换是其他协议的 1.7 倍（19 000 vs 11 400 / 30 s），且集中在单个工作线程。

## 尚未解决 / 本阶段不能回答

- **per-source 与 per-viewer 成本无法分离**：矩阵固定 1 source × N viewer，WHEP 的 0.084 核拟合截距究竟属于转码（per-source）还是 SRTP（per-viewer）无法判定 → 阶段 4。
- **容量边界未知**：线性外推（WHEP ≈ 940、FLV ≈ 1 700、HLS ≈ 9 700 viewer 耗尽 4 核）只是算术结果，未验证排队、内存与尾延迟在更高并发下的行为 → 阶段 5。
- **瓶颈归因未做**：上述"原因"（HTTP 封装、UDP syscall、转码路径）均为待验证假设，本阶段没有 CPU 采样证据 → 阶段 6。
- 端到端播放延迟、卡顿、音视频同步未度量；本阶段只证明媒体完整、速率正确。
- 单一 fixture（720p30 / 1.53 Mbps / 30 fps），结论不能直接换算到其他码率/帧率。

## 工具与环境限制（延续阶段 1，未新增）

- **off-CPU 采样仍 BLOCKED**：`offcputime-bpfcc` 受 PyPI `bcc` 包遮蔽与内核符号 `finish_task_switch.isra.0` 无法挂载 kprobe 影响；未修改全局内核配置。
- **`jeprof` 在 TEST 缺失**：堆分析需从 DEV 打包，阶段 6 前处理。
- 阶段 3 未使用 off-CPU 与堆分析，上述阻塞不影响本阶段结论。

## 产物与清理

- TEST：`phase3-protocols/`（31 MB，含每用例 `result.json` 与客户端产物）、`phase3-probe/`（1.9 MB）、`phase3-baseline/`（764 KB）、`extract.jsonl`、`aggregate.json`、`aggregate.csv`。
- 脚本：`pi_phase32b.py`（DUT 扩展基线采样）、`pi_phase32c.py`（publisher/signaling 拆分）、`pi_phase3.py`（矩阵执行）、`pi_report.py`（指标提取）、`pi_agg.py`（聚合）。
- 清理状态：矩阵结束后 `pgrep media_server` 为空，命名空间内监听端口 0，无 `perf_run` / `_fanout` / `ffmpeg` 残留。
- 保留：runtime bundle、fixture、命名空间（阶段 4 继续复用）、历史报告。

## 待评审事项

1. 阶段 4 的多 source 矩阵维度（1/2/4/8 source × 固定 viewer 数）与 viewer 档位选择是否按上述"分离 per-source / per-viewer 成本"的目标设定。
2. WHEP 固定成本与 GB UDP/TCP 差异是否列为阶段 6 的必查项。
3. 阶段 5 容量搜索的 CPU 预算口径（例如"服务端 ≤4 核"还是"≤6 核"）需要确认，它直接决定容量数字。
