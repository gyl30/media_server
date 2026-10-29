# 阶段 4C：80 viewer 下的 1 source 与 8 source 对照

运行：`20261012T043240Z-76355e4` · TEST `<TEST_HOST>` · 命名空间 `msperf-20261012T043240Z-76355e4`
被测：`media_server` HEAD `76355e435a766ed6d6c58be5dc315d14256b4e65`，产物 SHA256 `e8f8e298…`
范围：6 个协议（RTMP、HTTP-FLV、HLS、GB UDP、GB TCP active、GB TCP passive）× source ∈ {1,8} × 固定 80 viewer × 3 次重复 = **36 例，全部 PASS，退出码 0**。
选择依据：固定 viewer 只变 source 才能把每 source 成本从每 viewer 成本中分离；阶段 3 已给出各协议 1 source 的每 viewer 斜率，因此只需补 1 与 8 两个 source 点。RTSP 与 WHEP 不在此列（RTSP 与 FLV 在阶段 3 同价，WHEP 已在 4B 做完整二维）。
原始数据：TEST `phase4c/`、聚合 `phase4c_grid.json`。

## 服务端 CPU（核，3 次中位数）

| 协议 | S=1 | S=8 | ΔCPU/source | 参考差值（ΔCPU/source − 5.99，基线仅来自 RTMP 输入） |
| --- | --- | --- | --- | --- |
| RTMP | 0.17050 | 0.23425 | **9.11 mCPU** | **+3.11 mCPU** |
| HTTP-FLV | 0.21321 | 0.26262 | 7.06 mCPU | +1.06 mCPU |
| HLS | 0.04004 | 0.08876 | 6.96 mCPU | +0.96 mCPU |
| GB TCP active | 0.17680 | 0.22289 | 6.58 mCPU | +0.58 mCPU |
| GB TCP passive | 0.17846 | 0.22021 | 5.96 mCPU | −0.04 mCPU |
| GB UDP | 0.23354 | 0.26428 | **4.39 mCPU** | −1.61 mCPU |
| （对照）WHEP，阶段 4B | 0.42112 | 0.77519 | **50.6 mCPU** | **≈ +45 mCPU** |

重复波动：0.00%–5.84%（HLS S=1 绝对值最小 0.039–0.041 核，相对极差最大 5.84%；其余 ≤ 3.0%）。

**结论：除 WHEP 外，各协议的每 source 成本与"纯接入成本"（阶段 4A 在 RTMP 输入上测得的 5.99 mCPU/source）无法区分。** 该基线来自 RTMP 输入的零 viewer 基线，不能无条件用于扣除其他输入路径的全部接入成本；本表的"参考差值"只用于量级判断，各协议的 source 增量应结合其自身测试路径与重复波动（0.0%–5.8%）解读。 也就是说 RTMP/FLV/HLS/GB 的转发路径没有可测的 per-source 固定开销，成本都在 viewer 维度；只有 WHEP 有显著的 per-source 分量（约 45 mCPU，是其他协议的 5–10 倍）。

## 与阶段 3 的独立交叉验证

用阶段 3 的 1 source 拟合（V=1/10/50/100）外推 V=80，与本阶段实测 S=1 对比：

| 协议 | 阶段 3 外推 | 阶段 4C 实测 | 偏差 |
| --- | --- | --- | --- |
| RTMP | 0.17119 | 0.17050 | −0.40% |
| HTTP-FLV | 0.21475 | 0.21321 | −0.71% |
| HLS | 0.04022 | 0.04004 | −0.45% |
| GB UDP | 0.23366 | 0.23354 | −0.05% |
| GB TCP active | 0.17797 | 0.17680 | −0.65% |
| GB TCP passive | 0.17886 | 0.17846 | −0.22% |

两个独立批次（不同日期、不同运行、不同 viewer 档位）在 6 个协议上一致到 0.7% 以内，说明阶段 3 的每-viewer 斜率与阶段 4 的 source 维度测量都可靠。

## 内存：HLS 是唯一随 source 数显著增长的协议

| 协议 | PSS @S=1 | PSS @S=8 | ΔPSS/source |
| --- | --- | --- | --- |
| **HLS** | 26.06 MiB | **61.14 MiB** | **+5.01 MiB** |
| GB TCP active | 23.62 | 26.51 | +0.41 MiB |
| GB TCP passive | 23.79 | 26.36 | +0.37 MiB |
| GB UDP | 22.82 | 25.12 | +0.33 MiB |
| RTMP | 35.42 | 36.89 | +0.21 MiB |
| HTTP-FLV | 32.32 | 33.08 | +0.11 MiB |
| （对照）WHEP，阶段 4B | 43.24 | 51.55 | +1.15 MiB |

HLS 的 CPU 最省（每 source 仅 7.0 mCPU），但**每增加一路 source 要多占 5 MiB**（分片缓冲），是其他协议的 10–50 倍。容量规划若只看 CPU 会低估 HLS 的 source 维度成本。

## 线程、句柄与调度活动

| 协议 | 线程 S=1→S=8 | FD S=1→S=8 | 上下文切换 /30 s | 服务端单线程最大 |
| --- | --- | --- | --- | --- |
| RTMP | 16→16 | 113→120 | 11 529→21 513 | 0.026→0.032 |
| HTTP-FLV | 16→16 | 113→120 | 11 439→19 132 | 0.031→0.035 |
| HLS | **13**→16 | 113→120 | 4 473→14 133 | 0.009→0.022 |
| GB UDP | 10→16 | **193→200** | 11 408→20 814 | 0.033→0.037 |
| GB TCP active | 10→16 | 113→120 | 11 424→21 338 | 0.026→0.032 |
| GB TCP passive | 10→16 | 113→120 | 11 417→19 995 | 0.027→0.030 |

- FD 随 source 严格 +1（与阶段 4A 一致）；GB UDP 额外多 80 个 FD（每 viewer 一个本地 UDP socket），与阶段 3 相同。
- 上下文切换随 source 数近似线性上升（约 +1.1 k/30 s per source），与阶段 4A 观察到的"每 source +2 线程（上限 16）"一致；这是所有协议共有的现象，不是某个协议的特性。
- HLS 在 S=1 时线程数就是 13（其他协议 10），说明分片生成路径本身会额外占用线程。
- 所有配置下服务端单线程最大 ≤ 0.037 核，8 个工作线程远未饱和。

## 媒体健康（每个 source 都在真实推进）

36 例全部 `ready == requested == 80`、覆盖率 ≥ 0.9993、无失败。逐 viewer 级证据：

| 协议 | 每 viewer 证据（80 个 viewer 全量的最小值） |
| --- | --- |
| GB UDP / TCP active / TCP passive | 最低 **222.8 pkt/s、204.0 KB/s**（每个 viewer），`non_progressing_ssrcs = 0`、`sequence_gaps = 0`、`tcp_disconnects = 0` |
| HLS | 每个 viewer 恰好 **15 个分片 / 6 523 600 字节**（80 个 viewer 完全一致，无缺片） |
| RTMP / HTTP-FLV | 客户端逐 viewer 推进计数 `non_progressing_viewers = 0`，稳态 29.9 视频消息/s + 46.9 AAC/s |

viewer 与 source 的对应关系是确定且可复核的：`perf_run` 以 `sources[index % N]` 轮询分配，`result.json` 保存了 `source_ids` 顺序，客户端凭据文件按同一顺序落盘。因此"每个 viewer 都在推进"直接蕴含"每个 source 的流都在推进"。WHEP 的逐 source 直接归属取证见阶段 4B 文档。

## 客户端余量

| 协议 | 客户端 CPU 合计 S=1→S=8 | 客户端单线程最大 S=1→S=8 |
| --- | --- | --- |
| GB TCP active | 0.605→**0.694** | 0.608→**0.695** |
| GB TCP passive | 0.606→0.684 | 0.607→0.700 |
| GB UDP | 0.501→0.568 | 0.503→0.593 |
| HTTP-FLV | 0.241→0.269 | 0.243→0.280 |
| RTMP | 0.134→0.169 | 0.136→0.168 |
| HLS | 0.101→0.098 | 0.104→0.102 |

8 个客户端核上最高 0.69 核（8.7%）。需要记录的是 **GB 客户端是单进程 asyncio，其事件循环线程已达 0.70 核**：在 80 viewer 时接近单线程上限。这不影响本阶段的服务端结论（服务端单线程仅 0.03 核），但**阶段 5 若把 GB 协议推到更高 viewer 数，客户端会先于服务端饱和**，届时必须按 `CLIENT_RESOURCE` 判定而不是服务端容量。

## 环境与安全预算

36 例全程：PSI cpu/memory/io 0.00，`queue_full = 0`、`udp_drops = 0`、`No buffer space = 0`；可用内存最低 84.4 GB、可用磁盘最低 68.3 GB、DUT PSS 峰值 61.1 MiB（HLS 8 source），均远低于预算。

## 不能解释的现象（记录，不预设根因）

1. **RTMP 是唯一有可测 per-source 转发成本的流式协议**（+3.1 mCPU/source），同内容的 HTTP-FLV 只有 +1.1。两者媒体内容完全相同，差异只在封装/会话层。
2. **GB UDP 的 per-source 成本（4.39 mCPU）低于通用接入基线（6.0 mCPU）**，说明"接入成本"并非在所有输入路径上恒定，阶段 4A 的基线只适用于 RTMP 输入。本阶段所有"扣除接入成本"的差值都应理解为近似量级，不是精确分解。
3. HLS 的 CPU 与内存对 source 数的敏感度方向相反（CPU 最省、内存最贵），需要阶段 5 在容量口径中同时约束两者。
