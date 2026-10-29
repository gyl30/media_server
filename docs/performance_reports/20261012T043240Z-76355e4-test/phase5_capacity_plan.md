# 阶段 5 容量测试计划

运行：`20261012T043240Z-76355e4` · TEST `<TEST_HOST>` · 命名空间 `msperf-20261012T043240Z-76355e4`
被测：`media_server` HEAD `76355e435a766ed6d6c58be5dc315d14256b4e65`，产物 SHA256 `e8f8e298…`，`dirty=false`
执行器：`pi_phase5.py`（逐档调用 `bench/perf_run.py`，判据与测量窗口不变；额外采样每核繁忙率、DUT 实时 CPU、系统 PSI）

## 一、开始前检查（阶段 4 统计口径）

两处均已用原始数据重算并修正文档，未重跑阶段 3/4：

1. **单位错误**：阶段 4 报告的 WHEP 低 viewer 斜率应为 **9.38 mCPU/viewer（V=1→10）** 与 **3.91 mCPU/viewer（V=10→100）**，原文误写为 µCPU/viewer。数值本身正确：V=1 0.06338、V=10 0.14779、V=100 0.49973 → (0.14779−0.06338)/9 = 0.009379 核/viewer。3.91 mCPU/viewer 与 4B 的 3.885 mCPU/viewer 一致（差 0.6%）。
2. **残差百分比**：最大绝对残差 +0.00267 核出现在 (S=4,V=16)，该点观测值 0.32864 → **相对观测值 0.81%**；原文的 0.34% 是以全场最大观测值 0.77519 为分母。已统一为"相对各自观测值"，并写明符号约定（观测 − 预测）。全 8 点相对残差 ≤0.81%，RMS 0.00124 核。
3. **适用范围**：模型 `CPU ≈ 0.06108 + 0.05080·S + 0.003855·V` 只在 S=1/2/4/8、V=16/80 网格内验证；不用于 V=0，也不作为高负载外推公式，本轮仅用于规划下一档。
4. **接入基线范围**：5.99 mCPU/source 只来自 RTMP 输入的零 viewer 基线，不用于扣除其他输入路径的接入成本；阶段 4C 的差值列已改为"参考差值"。

## 二、客户端与负载生成器能力预检

### 负载生成器线程模型

| 客户端 | 进程内线程 | 现有并行手段 |
| --- | --- | --- |
| `http_flv_fanout` / `rtmp_fanout` / `rtsp_fanout` | 单 `io_context`，单线程 | `perf_run --group-size` 拆多进程 |
| `whep_fanout` | 单 `io_context` + `--io-threads`（`perf_run` 固定传 1） | 同上 |
| `gb_sender_fanout.py` | 单 asyncio 事件循环，无 executor | 同上 |
| `hls_client.py` | 单 asyncio 事件循环 | 同上 |

`perf_run` 的客户端判据本身就以**每进程 0.9 核**和**单线程 0.9 核**为界（`CLIENT_RESOURCE`），因此"总 CPU 空闲但某线程先饱和"会被工具直接识别。本轮不重构 bench，只使用已有的 `--group-size` 分摊客户端工作，媒体健康判据不变。

### 客户端切分口径校准（本轮实测，6 例，全部 PASS）

| 档位 | 1 进程 | 4 进程 | 同场差异 | 与阶段 3/4 同档对比 |
| --- | --- | --- | --- | --- |
| FLV 100 viewer | 0.25592 | 0.27084 | **+5.9%** | 1 进程 0.25592 vs 阶段 3 的 0.25694（−0.4%，区间内） |
| WHEP 80 viewer | 0.42911 | 0.42539 | −0.9% | 1 进程 0.42911 vs 阶段 4B 的 0.42112（**+1.9%，跨会话漂移**） |
| GB UDP 80 viewer | 0.22853 | 0.23154 | +1.3% | 1 进程 0.22853 vs 阶段 4C 的 0.23354（−2.1%） |

结论：

1. **同配置复跑可复现**（FLV −0.4%），环境未变；但**跨会话漂移约 ±2%**（WHEP +1.9%、GB −2.1%），因此阶段 5 的对比一律以同场运行为准，并在会话首尾各放一次锚点档（WHEP S=1 V=80）。
2. **拆分客户端进程对 DUT 侧的影响**：WHEP −0.9%、GB UDP +1.3%（在跨会话漂移内），**FLV +5.9%（明显超出）**。因此 FLV 在单进程仍能承载的档位（≤300 viewer，最热线程 <0.7 核）保持 1 进程，超过 300 才拆分，并在 300 viewer 处做一次"1 进程 vs 多进程"桥接，量化该配置台阶。
3. **GB 客户端单事件循环确为串行瓶颈**：仅 80 viewer 时最热线程就达 **0.525 核**（单进程），折算约 155 viewer/核。因此 GB 一律 `--group-size 60`（每进程约 0.38 核），否则客户端会先于 DUT 饱和。
4. 各客户端每 viewer CPU（阶段 3/4 实测折算）：GB TCP 6.4、GB UDP 5.8、FLV 2.97、WHEP 1.85、RTMP 1.69、HLS 1.18 µCPU → 决定下表的 `--group-size`，使每进程最热线程目标 ≤0.5 核。

### 环境约束：FD 软限制 1024（本轮不修改）

`ulimit -n = 1024`（硬限制 1 048 576），生产代码与 `perf_run` 都不调整 `RLIMIT_NOFILE`。DUT 的 FD 实测约为 `33 + viewer`（GB UDP 为 `113 + 2×viewer`，每 viewer 一个发送 socket）：

| 协议 | FD 约束下的可测上限 | 达到 4 核预警所需 viewer（阶段 4 模型，仅用于规划） |
| --- | --- | --- |
| WHEP S=1 | ≈ 990 | ≈ 1015 |
| WHEP S=8 | ≈ 985 | ≈ 960 |
| FLV / RTMP | ≈ 990 | ≈ 1700 / 2150 |
| HLS | ≈ 990 | ≈ 9700 |
| GB TCP | ≈ 990 | ≈ 2050 |
| **GB UDP** | **≈ 455** | ≈ 1490 |

**因此在不修改 FD 软限制的前提下，没有任何协议能到达 4 核预警线；约束点是 FD，而不是 DUT 的 CPU 或内存。**

> **执行结果（已批准并实施）**：用户批准把测试进程树的 FD 软限制提升到 65536（仅测试启动的进程，不改全局 sysctl/PAM/systemd，外层 shell 仍为 1024），运行中读取 DUT 的 `/proc/<pid>/limits` 已验证为 65536。档位上限因此从 800 提升到 1500（GB UDP 不再受 440 限制）。详见 `phase5_capacity.md` §2。

## 三、负载台阶与判据

停止规则（沿用既有口径）：DUT CPU 达 4 核预警并缩小增量；达 6 核立即停止并记 `BUDGET_STOP`（`pi_phase5.py` 连续 3 次采样超线即终止该档）；客户端每进程 0.9 核或单线程 0.9 核 → `CLIENT_RESOURCE`；publisher 异常 → `UPSTREAM_RESOURCE`；PSI/内存/磁盘/PSS/队列按既有预算；本轮单档不超过 1500 viewer，不做 soak。

分类映射：`perf_run` 的 `capacity_boundary=HEALTHY + status=PASS` → `PASS`；`CLIENT_RESOURCE` → `CLIENT_RESOURCE`；`UPSTREAM_FAILED` → `UPSTREAM_RESOURCE`；`SERVER_UNSTABLE` → `DUT_FAILURE`（需 ≥2 次独立复现）；预算触发 → `BUDGET_STOP`；runner 未启动/无 `result.json` → `TEST_INFRA_FAILURE`。未确认的失败不建立边界。

| 阶段 | 协议 | source | viewer 台阶 | group-size | 探索/正式 |
| --- | --- | --- | --- | --- | --- |
| 锚点 | WHEP | 1 | 80 | 80 | 会话首尾各 1 次，量化跨会话漂移 |
| 5A | WHEP | 1 | 200 → 400 → 600 → 800 | 200 | 每档 1 次探索；最高健康档 + 中间档各 3 次正式 |
| 5B | WHEP | 8 | 200 → 400 → 800 | 200 | 同上；另加 S=8 V=160 `--group-size 1` 做逐 source 直接取证 |
| 5C | GB UDP | 1 | 160 → 300 → 440 | 60 | 探索 + 最高健康档 3 次 |
| 5C | GB TCP active / passive | 1 | 160 → 300 → 500 → 700 | 60 | 同上 |
| 5D | RTMP | 1 | 200 → 400 → 800 | 250 | 同上 |
| 5D | HTTP-FLV | 1 | 200 → 300（1 进程）→ 300（gs150 桥接）→ 400 → 600 → 800 | 1 / 150 | 同上 |
| 5D | HLS | 1 | 200 → 400 → 800 | 300 | 同上；另加 S=8 V=800 观察每 source 分片内存 |

每档记录：连接/推进 viewer 数、视频 RTP 与 Opus 包、SSRC 进展、sequence gap、UDP/TCP 队列与丢包、DUT CPU/PSS/RSS/FD/线程/PSI、客户端总 CPU 与最热线程、publisher 存活、每核繁忙率、退出码与分类。CPU 一律使用窗口首末累计值差值。

预计工作量：探索档约 32 例（高档位单例 80–95 s）≈ 50 min；正式重复约 24 例 ≈ 35 min；合计约 90 min 机器时间。

## 四、阶段 6 输入（本轮只记录）

P1 WHEP per-source CPU：需要 4 个稳定负载 —— 小规模对照、WHEP S=1、WHEP S=8（总 viewer 相同）、高 viewer 但余量充足。P2 GB UDP/TCP（必须避开客户端事件循环饱和的负载）。P3 RTMP/FLV 仅在容量或 profile 显示有收益空间时处理。
