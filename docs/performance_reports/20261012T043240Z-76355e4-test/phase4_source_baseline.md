# 阶段 4A：多 source 严格零 viewer 发布基线

运行：`20261012T043240Z-76355e4` · TEST `<TEST_HOST>` · 命名空间 `msperf-20261012T043240Z-76355e4`
被测：`media_server` HEAD `76355e435a766ed6d6c58be5dc315d14256b4e65`，产物 SHA256 `e8f8e298…`
脚本：`pi_phase4a.py`（1/2/4/8 source × 3 次重复，共 12 次独立启动，全部完成）
原始数据：TEST `phase4a/`（每例含 1 Hz 原始采样、控制面快照、perf_publish 探针日志）

## 方法

- 每例独立启动 signaling + DUT（`taskset 0-7`、`--threads 8`）+ `perf_publish`（真实信令，N 路 RTMP fixture）。
- `perf_publish` 在宣布 generation 就绪前，对**每个 source** 做真实 FLV 播放探针；探针客户端在测量窗口外退出。
- 就绪后静置 8 s，再取 30 s 窗口，1 Hz 采样 DUT、每个 publisher、signaling 三个进程组。
- CPU 一律用**窗口首末点差值 / 墙钟**（阶段 3 的 1 Hz 中位数估计量已证实有偏，见 `phase3_baseline.md`）。

0 viewer 与多 source 生效的判定证据（12 次全部满足）：

| 证据 | 要求 | 实测 |
| --- | --- | --- |
| `ss` 11935 ESTABLISHED | = N | 1/2/4/8 全部相符 |
| `ss` 18080 / 18554 ESTABLISHED | = 0 | 全部为 0 |
| 控制面 `/receivers` | 恰好 N 个互不相同的 `stream_id`，与请求一致 | `count==distinct==N`、`matches_requested=true` |
| 每个 source 的真实媒体探针 | 就绪前逐 source 通过 | `probe_evidence` 非空且 `ready=true` |
| publisher 存活 | 整个窗口存活 | `publishers_alive_after_window=true` |

## DUT 成本（3 次中位数）

| source | CPU 核 | CPU 相对上一档 | PSS MiB | RSS MiB | FD | 线程 |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 0.00617 | — | 19.58 | 22.4 | 33 | 10 |
| 2 | 0.01163 | +0.00546 | 20.72 | 23.6 | 34 | 12 |
| 4 | 0.02353 | +0.00579 | 23.12 | 26.0 | 36 | 16 |
| 8 | 0.04809 | +0.00599 | 24.15 | 27.1 | 40 | 16 |

重复波动（同配置 3 次的相对极差）：1 source 5.5%、2 source 0.1%、4 source 5.8%、8 source 2.1%；绝对极差 ≤ 0.0014 核。

### 每 source 接入成本

- **CPU 线性**：端点斜率 5.99 mCPU/source，逐档差分 5.46 / 5.79 / 5.99 mCPU，拟合截距 ≈ +0.0002 核。即 RTMP 接入成本几乎完全按 source 线性增长，没有可测的固定分量，也没有共享缓存带来的规模效应。
- **内存不是线性**：PSS 逐档 +1.14、+1.20、+0.34 MiB（1→2、2→4、4→8），端点斜率 0.65 MiB/source。前几档增量是后一档的 3 倍，符合分配器在已有池上复用的行为；用 1→8 的端点斜率外推会低估小集群成本、高估大集群增量。
- **FD 严格 +1/source**（33→40），与每路一条 TCP 连接一致。
- **线程 +2/source，到 4 source 后封顶在 16**（10→12→16→16）。8 个工作线程的 DUT 在 4 source 时达到 16 线程上限，说明每 source 的额外线程来自一个有上限的池，而不是无界地按连接分配。这是本阶段唯一与"线性"明显不符的资源维度，具体归属留给阶段 6。

## 客户端侧成本（不计入 DUT）

| source | publisher(ffmpeg) CPU 合计 | publisher PSS 合计 | signaling CPU | signaling PSS |
| --- | --- | --- | --- | --- |
| 1 | 0.0113 核 | 26.7 MiB | 0.0007 核 | 13.8 MiB |
| 2 | 0.0222 核 | 39.3 MiB | 0.0003 核 | 14.0 MiB |
| 4 | 0.0450 核 | 60.9 MiB | 0.0007 核 | 14.3 MiB |
| 8 | 0.0874 核 | 102.3 MiB | 0.0007 核 | 14.3 MiB |

publisher 约 11 mCPU/路、PSS 随路数亚线性（共享映射页只计一次）；signaling 在无请求时保持 0 核。8 source 时客户端合计约 0.09 核，占 8 个可用核的 1%，远未成为限制。

## 结论

1. **每 source 接入成本 ≈ 6.0 mCPU、+1 FD、+2 线程（上限 16）、+0.3~1.2 MiB PSS**，与 viewer 数无关。
2. 该数值是后续多 source 对照的**参考基线**，但它只在 RTMP 输入、零 viewer 的路径上测得，不能无条件用于扣除其他输入协议（GB 输入路径实测更低，见阶段 4C）。判断某协议自身的 per-source 成本时，应优先比较同一输入路径下不同协议的 ΔCPU/source 差异，而不是机械减去 5.99 mCPU。
3. 修正了阶段 3 的 publish-only 数值（0.0089 → 0.0062 核），原因是采样估计量偏差，不是行为变化。
