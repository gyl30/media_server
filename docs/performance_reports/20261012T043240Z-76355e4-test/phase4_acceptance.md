# CHECKPOINT 4：阶段 4 验收

运行：`20261012T043240Z-76355e4` · TEST `<TEST_HOST>` · 命名空间 `msperf-20261012T043240Z-76355e4`
被测：`media_server` HEAD `76355e435a766ed6d6c58be5dc315d14256b4e65`，产物 SHA256 `e8f8e298…`，`dirty=false`
明细：`phase4_source_baseline.md`（4A）、`phase4_whep_source_viewer.md`（4B）、`phase4_protocol_source_scaling.md`（4C）

## 结论

阶段 4 目标全部达成，**76 个用例全部 PASS，0 失败、0 中止、0 资源预算触发**（4A 12 次独立启动 + 4B 28 例 + 4C 36 例）。

阶段 3 遗留的问题——"固定成本属于 per-source 还是 per-viewer"——已用正交方向的有限差分给出确定答案：

1. **WHEP 的固定成本是 per-source：50.8 mCPU/source**，在 16 与 80 viewer 下分别测得 50.4–52.9 与 50.4–50.7 mCPU，与 viewer 数无关；以阶段 4A 的 RTMP 输入基线 5.99 mCPU/source 为参考（该基线只适用于 RTMP 输入路径），WHEP 自身约 **45 mCPU/source**。
2. **WHEP 的每 viewer 成本 3.885 mCPU（3885 µCPU），与 source 数无关**（1/2/4/8 source 下 3.81–3.89 mCPU），可加模型 `a + b·S + c·V` 残差 RMS 0.0012 核、最大残差相对该点观测值 0.81%，无需交互项。该模型仅在 S=1/2/4/8、V=16/80 实测范围内成立。
3. **除 WHEP 外没有可测的 per-source 转发成本**：RTMP 9.11、FLV 7.06、HLS 6.96、GB TCP active 6.58、GB TCP passive 5.96、GB UDP 4.39 mCPU/source，与"纯接入 6.0 mCPU/source"基线无法区分。
4. **HLS 的 source 维度成本在内存而不是 CPU**：+5.01 MiB/source（其他协议 0.1–0.4，WHEP 1.15），8 source 时 PSS 61.1 MiB 为全场最高。

## 执行量

| 项目 | 内容 | 墙钟 |
| --- | --- | --- |
| 4A | 1/2/4/8 source 严格零 viewer 基线 × 3 次 = 12 次独立启动 | 15.1 min |
| 4B | WHEP sources{1,2,4,8} × viewers{16,80} × 3 = 24 例；另 4 例 `--group-size 1` 逐 source 取证 | 22.6 min（单例最长 64.7 s） |
| 4C | 6 协议 × source{1,8} × 80 viewer × 3 = 36 例 | 28.9 min（单例最长 48.3 s） |

配置与阶段 3 一致：同 fixture、`--workers 8`、DUT `taskset 0-7`、客户端 `taskset 8-15`、warmup 10 s、测量 30 s、`min-free-memory 8192 MiB / min-free-disk 2048 MiB / max-server-pss 16384 MiB`。4A 的 CPU 使用窗口首末点差值；4B/4C 的服务端指标由 `perf_run` 采集（本就是首末点差值）。

## 隔离与安全预算

| 约束 | 要求 | 实测 |
| --- | --- | --- |
| DUT / 客户端 CPU | 0-7 / 8-15，SMT 兄弟核排除 | 全程保持，无重叠（`perf_run` 会主动校验并拒绝重叠） |
| 网络 | 专用命名空间 | 复用 `msperf-…`，未新建；结束后监听端口 0、无残留进程 |
| 系统 PSI | 触发即停 | cpu / memory / io 全程 0.00 |
| 可用内存 / 磁盘 | ≥ 8 192 / ≥ 2 048 MiB | 最低 84.2 GB / 68.3 GB |
| DUT PSS | ≤ 16 384 MiB | 峰值 61.1 MiB（HLS 8 source） |
| 服务端异常证据 | queue_full / udp_drops / No buffer space | 全部为 0 |
| 全局 sysctl / ulimit / cgroup | 不修改 | 未修改；未动主仓库工作区 |

## 重复波动

| 批次 | 相对极差 | 说明 |
| --- | --- | --- |
| 4A | 0.1%–5.8% | 绝对极差 ≤ 0.0014 核（绝对值本身只有 0.006–0.048 核） |
| 4B | 0.2%–2.7% | 8 个网格点全部 ≤ 2.7% |
| 4C | 0.0%–5.8% | 最大 5.8% 出现在 HLS S=1（0.0387–0.0410 核），其余 ≤ 3.0% |

## 媒体健康

- 4B 主矩阵 24 例：`requested == ready == progressing`、覆盖率 ≥ 0.999995、`unprotect_failures = 0`。
- 4B 逐 source 直接取证（`--group-size 1`，4 例）：**每一个不同 `stream_id` 都有独立推进的音视频**，视频 176–185 RTP 包/s、音频 50.0 Opus 包/s、每 viewer 218.4 KB/s，各 source 取值一致，无停滞、无超速。
- 4C 36 例：全部 80/80 就绪。GB 三种传输的 **80 个 viewer 中最低速率 222.8 pkt/s、204.0 KB/s**，`non_progressing_ssrcs = 0`、`sequence_gaps = 0`、`tcp_disconnects = 0`；HLS 每个 viewer 恰好 15 个分片 / 6 523 600 字节（完全一致，无缺片）；RTMP/FLV `non_progressing_viewers = 0`。
- viewer→source 映射确定且可复核（`sources[index % N]` + `result.json` 的 `source_ids` + 凭据文件同序落盘）。

## 客户端余量

| 场景 | 客户端 CPU 合计（8 核可用） | 客户端单线程最大 |
| --- | --- | --- |
| 4B WHEP 8 source × 80 viewer | 0.213 核 | 0.216 |
| 4B 逐 viewer 取证（80 个客户端进程） | 0.635 核 | — |
| 4C GB TCP active 8 source × 80 viewer | 0.694 核 | **0.695** |
| 4A 8 source publisher 合计 | 0.087 核 | — |

**唯一需要记录的客户端风险：GB 客户端是单进程 asyncio，其事件循环线程在 80 viewer 时已达 0.70 核。** 阶段 5 若把 GB 协议推向更高并发，客户端会先于服务端饱和，必须按 `CLIENT_RESOURCE` 判定，不能当作服务端容量。

## 对阶段 3 的修正

阶段 3 的严格 publish-only CPU 由 0.0089 核修正为 **0.0062 核**。原因是采样估计量偏差：1 Hz 相邻差值的中位数在"每秒不足 1 个时钟节拍、实际间隔约 1.11 s"的低负载进程上系统性高估约 1.4 倍；改用窗口首末点差值后由阶段 4A 的 12 次运行复现。`perf_run` 的服务端指标始终使用首末点差值，**阶段 3B 协议矩阵与阶段 4 全部数据不受影响**。已同步修正 `phase3_baseline.md`、`phase3_protocol_comparison.md`、`phase3_acceptance.md` 中的相关数值。

## 不能解释的现象（仅记录，不预设根因）

1. **约 0.055 核的固定成本只在存在 WHEP viewer 时出现**：可加模型的截距 0.061 核在 V=0 处不成立（实测 publish-only 0.0062 核），且不随 source 数或 viewer 数变化。
2. **每 viewer 成本在 V<10 区间存在曲率**：合并阶段 3 与 4B 的 1 source 数据，1→10 的斜率 9.38 mCPU/viewer，10→100 只有 3.91 mCPU/viewer（初版误写为 µCPU/viewer，已修正）。小规模部署不能用每-viewer 常数外推。
3. **RTMP 的 per-source 转发成本 +3.1 mCPU，同内容 HTTP-FLV 只有 +1.1**；差异只在封装/会话层。
4. **GB UDP 的 per-source 成本（4.39 mCPU）低于 RTMP 输入基线（6.0 mCPU）**，说明"接入成本"随输入路径不同而变化，阶段 4A 的基线只适用于 RTMP 输入；本阶段所有"扣除接入成本"的差值应视为量级估计。
5. **每 source +2 线程、在 4 source 时封顶 16**；HLS 在 1 source 时线程数已为 13。归属未定。
6. HLS 的 CPU 与内存对 source 数的敏感度方向相反（CPU 最省、内存最贵）。

## 阶段 5 / 阶段 6 记录（本轮不执行）

- 阶段 5 口径已确认：**4 核为预警参考、6 核为本轮探索停止线，二者都不作为 `SERVER_UNSTABLE` 或最大容量判据**；本轮未执行任何容量搜索。
- 阶段 6 优先级：**WHEP 的 CPU 成本（含上述第 1、2 条）与 GB UDP/TCP 差异为优先诊断对象**，FLV/RTMP 差异为次级对象。
- 阶段 1 记录的环境缺陷仍未处理：off-CPU 采样 BLOCKED（PyPI `bcc` 遮蔽 + 内核符号 `finish_task_switch.isra.0`）、`jeprof` 在 TEST 缺失。阶段 6 前需要处理，否则堆与阻塞路径无法归因。

## 产物与清理

- TEST 保留：`phase4a/`（1.6 MB，含 1 Hz 原始采样）、`phase4b/`（21 MB）、`phase4c/`（18 MB）、首次 4A 尝试 `phase4a-run1-quantized-cpu/`（保留以说明估计量问题）、`phase4b_grid.json`、`phase4c_grid.json`、`plan4b.jsonl`、`plan4c.jsonl`。
- 脚本：`pi_phase4a.py`（多 source 零 viewer 基线）、`pi_phase4.py`（二维网格与成本分解）、`pi_phase4v.py`（逐 source 媒体取证）；`pi_phase3.py` 增加 `extra_args` 透传（用于 `--group-size`），`pi_phase4b/4c` 复用同一执行器，判据未改。
- 清理状态：`media_server` / `ffmpeg` / 客户端进程均无残留，命名空间内监听端口 0。runtime bundle、fixture、命名空间保留待阶段 5 复用。
- 主仓库工作区未改动，未 push。

## 待评审事项

1. 上述第 1、2 条（WHEP 固定成本与低 viewer 曲率）是否作为阶段 6 的首要诊断目标，以及是否需要为 off-CPU 采样修复 TEST 环境（安装到 venv 之外或改用 `bpftrace` 包装）。
2. 阶段 5 的容量口径是否需要同时约束内存维度（HLS 的 +5 MiB/source 使纯 CPU 口径失真）。
3. 阶段 5 是否需要为 GB 协议预留客户端余量（或提高 GB 客户端并发度），以避免把客户端饱和误判为服务端容量。
