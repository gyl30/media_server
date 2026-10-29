# media_server 协议与业务场景性能优化 A/B 验证（F1/F2、A1.1、A1.2、A3）

日期：2026-10-27。本报告覆盖 Phase 1 功能修复与体验基线、Phase 2 TCP 背压（A1.1）、Phase 3 GB TCP 合并写出（A1.2）、Phase 4 UDP 接收缓冲（A3）。

## 0. 结论速览

| 项 | 结论 | 提交 | 依据摘要 |
| --- | --- | --- | --- |
| F1 WHEP 拉流转 FLV/RTMP 半速 | 修复，ACCEPT | `cfe80a0` | base 3/3 FAIL（实时比 0.495、滞后 30 s）→ 2/2 PASS（1.000、58 ms）；音画偏差由 −13…−56 ms 漂移变为恒定 −56 ms |
| F2 HLS G.711 | 修复，ACCEPT | `3c6fddc` | base pcma/pcmu HLS 解码 FAIL → 仅视频 HLS PASS；其他输出保留 G.711 |
| A1.1 TCP 背压与写出 | ACCEPT | `9a1d603`、`46f0552` | F5 根因确认；RTMP 9000、RTSP 8500、FLV 8500 由 FAIL 变 PASS，PSS 14–15 GiB → 2.2–3.3 GiB；RTMP 6500 周期 −11% |
| A1.2 GB TCP 合并写出 | REJECT（不做独立改动） | 无 | A2 切片前后 GB 批量几乎不变；A1.1 后 GB TCP 已无单独瓶颈，create 中位数三版本一致 |
| A3 UDP 媒体输入接收缓冲 | ACCEPT（GB UDP、WHEP 拉流）；WHIP、RTSP UDP 为 INCONCLUSIVE | `28b8ede` | 默认缓冲下 GB UDP 300 / WHEP 拉流 100 共 4/4 轮突发溢出，256 KiB 后 0 丢包；WHIP 200 失败由单 worker 饱和引起，缓冲无效 |

容量（详见 §7）：

| 输出 | 稳定容量（≤ 7.2 核）BASE → FINAL | 最大持续交付 BASE → FINAL | 最大连接保持 BASE → FINAL |
| --- | --- | --- | --- |
| RTMP | ≥ 7500 → ≥ 8000 | < 8500 → ≥ 12000（14000 临界） | < 8500（开始断开）→ ≥ 14000 |
| RTSP | ≥ 7000 → ≥ 7500 | < 8000 → ≥ 11500 | ≥ 8000 → ≥ 13000 |
| HTTP-FLV | ≥ 7000 → ≥ 7500 | < 8500 → ≥ 13000 | ≥ 8500 → ≥ 13000 |
| 混合 6 协议 | ≥ 6600 → ≥ 6600（CPU −1.7%） | < 8400 → ≥ 9000 | < 8400 → ≥ 9000 |

常态负载下的主要收益是 CPU：RTMP −11.5%、FLV −3%、混合 −2%；体验（首帧、帧率、音画同步、控制响应）与 BASE 相当或略好（§8）。最大收益在过载：饱和后不再内存失控和断开，而是继续实时交付。最高负载点的滞后和控制延迟不适合作为常态运行点。

## 1. 版本、环境与工作区

| 项 | 值 |
| --- | --- |
| BASE HEAD | `6d87f96`（A2 dispatch 公平性），ELF `06f70315…`（bundle `rt-p1-base`） |
| FINAL HEAD | `28b8ede`，ELF `dd099cfc3555877389f50d7af61fdbca412486d8ebb00a2f61e5c4faa58a5d0f`（bundle `rt-final` = `rt-a3f`） |
| FINAL 工作区 | 干净（`git status` 无改动）；`build/media_server` 由该树构建，未 push |
| 构建 | gcc-16 RelWithDebInfo，Boost 1.92，仓库 `build/` 增量构建 |
| TEST | <TEST_HOST>，Xeon Silver 4216；服务端 8 个物理核（无 SMT 兄弟线程，`sibling_busy` ≤ 0.02），客户端在其他核；netns `198.19.0.10/11` |
| 内核网络参数 | `tcp_rmem 409600 8738000 2147483647`，`tcp_wmem 409600 6553600 2147483647`，`rmem_default 212992`，`rmem_max 2147483647`（rmem 在 6.8 内核不按 netns 隔离，未改动） |

中间 bundle（A/B 用）：

| bundle | 内容 | ELF |
| --- | --- | --- |
| `rt-a2A-c999003` | A2 之前 | `ef9aab26…` |
| `rt-p1-base` | BASE `6d87f96` | `06f70315…` |
| `rt-p1-f2` / `rt-a11A` | + F2 | `d85c6856…` |
| `rt-cfe80a0`（= `rt-p1-f1`） | + 工具 + F1，A1.1 的 A 端 | `5a5ce128…` |
| `rt-a11` | A + 仅 post→dispatch | `4f9a085c…` |
| `rt-a11g` | A + 合并写出（post） | `c6e4ec3f…` |
| `rt-a11gd` | A + 合并写出 + dispatch | `5f61039f…` |
| `rt-a11gdf` | + HTTP-FLV 合并 = `46f0552` | `eb26fb1c…`（本地按 `46f0552` 重建得到相同 SHA） |
| `rt-a3s` / `rt-a3l` | 实验：所有 UDP socket 256 KiB / 1 MiB | `36e7faf5…` / `c7066ab9…` |
| `rt-a3f` = `rt-final` | `28b8ede` | `dd099cfc…` |

方法约束：每个 A/B 对在同一 TEST、同一 fixture、同一参数下交替执行（A、B、B、A 或 A、B 交替）；每次 run 记录 `server_identity.exe` 与 ELF SHA、环境哨兵（`sibling_busy`、`server_max_cpu_busy`、MHz、C0 驻留、客户端 CPU）。判定阈值：实时比 ≥ 0.98、所有 viewer progressing、无服务端证据（`udp_drops`、`queue_full`、`no_buffer_space`）、PSS 预算 16 GiB。

原始数据：`raw/runs/`（各 run 的 `result.json`、客户端日志、`perf-stat.txt`），`raw/prof/wb-*`（bpftrace 系统调用计数），`tools/test-host/`（TEST 上的 `pr2.sh`、`a3.sh`、`wb.sh`、`sum.py` 及全部 job 脚本），`tools/ab.py`、`tools/gbctl.py`、`tools/avsum.py`（汇总脚本），`tests/`（transport 单元测试），`san/`（本地 sanitizer 回归）。

## 2. 与历史问题的对应

| 历史编号（8 核报告附录 B） | 本轮处理 |
| --- | --- |
| F1 WHEP 拉流转 FLV/RTMP 半速 | Phase 1 修复（`cfe80a0`） |
| F2 HLS G.711 音轨不可解码 | Phase 1 修复（`3c6fddc`） |
| F5 饱和时 RTMP/RTSP/FLV PSS 失控、viewer 被断开 | Phase 2 根因确认并修复（A1.1：`9a1d603`、`46f0552`） |
| A1（TCP 背压/合并） | 拆为 A1.1（通用 TCP 与 HTTP-FLV，ACCEPT）和 A1.2（GB TCP 专项，REJECT） |
| A3（UDP 接收丢包） | Phase 4（`28b8ede`，按 socket 角色生效） |
| F3/F4/F6 | 本轮未改动，见 §10 |

## 3. Phase 1：功能修复与体验基线

### 3.1 F2 HLS G.711

MPEG-TS 中 G.711 只能用 GB/T 私有 stream type，通用播放器和 ffmpeg 无法解码：BASE 的 HLS PCMA 音轨被 `ffprobe` 识别为 `mp3`、无可解码帧、192 条解码告警。修复与 Opus 处理一致：HLS 跳过 G.711 音轨只输出视频；其他协议仍输出 G.711。

| run | 结果 |
| --- | --- |
| `p1-g711-pcma-base-r1`、`p1-g711-pcmu-base-r1` | FAIL：HLS 解码失败 |
| `p1-g711-pcma-f2-r1`、`p1-g711-pcmu-f2-r1`、`p1-g711-h265pcma-f2-r1` | PASS：HLS 仅视频；其他输出 G.711 保留 |

### 3.2 F1 WHEP 拉流半速

根因：每个 RTCP SR 都重新锚定时间戳，使 pts 小幅回退；h264 dts 推断因此把 WebRTC 视频永久判为含 B 帧，dts 只按半速增长，WHEP 拉流转 RTMP/FLV 后实时比 0.495、滞后持续增长到 30 s。修复：每条轨只用首个 SR 对齐；WebRTC 视频（不含 B 帧）直接以 pts 作为 dts；RTSP 推流同样只用首个 SR，保留 dts 推断以支持 B 帧。

| 场景 | BASE | F1 |
| --- | --- | --- |
| WHEP 拉流 → RTMP/FLV（perf fixture） | 3/3 FAIL：实时比 0.495，滞后 30258–30284 ms | 2/2 PASS：实时比 1.000，滞后 58 ms |
| WHEP 拉流音画偏差（20 个 marker） | −56.5 / −36.0 / −13.2 ms（min/p50/max，漂移） | −56.0 恒定（4 轮） |
| WHEP 拉流视频 pts 最大间隔 | 46 ms | 34 ms |
| WHIP | −24.8 ms 恒定（两版本一致） | 同左 |
| RTSP TCP/UDP 推流、RTSP B 帧 | PASS | PASS（B 帧 fixture 通过 RTMP/FLV/HLS 输出验证） |

### 3.3 体验基线（单路，低负载）

`--av-probe 40`：每个输出用 ffmpeg 解码 40 s，统计首帧、帧率、pts 回退/间隔、到达停顿、音频 gap 和音画 marker 偏差。

| 输入 | 首帧 RTMP/FLV / RTSP / HLS | 帧率 | 视频间隔 max | 音画偏差 p50 | 备注 |
| --- | --- | --- | --- | --- | --- |
| RTMP | 2.29 / 1.87 / 0.09 s | 30.0 | 34 ms | +1.5 ms | 基准 |
| WHIP（F1） | 2.32 / 1.90 / 0.09 s | 30.0 | 34 ms | −24.8 ms | Opus→AAC 编码延迟，恒定 |
| WHEP 拉流（F1） | 2.32 / 1.88 / 0.09 s | 30.0 | 34 ms | −56.0 ms | 恒定，转码延迟 |
| RTSP TCP（F1） | 2.27 / 1.83 / 0.09 s | 30.0 | 34 ms | +6.8 ms（2.0–12.2 波动） | 56 次 ≤10 ms 音频 pts gap |
| RTSP UDP（F1） | 同 RTSP TCP | 30.0 | 34 ms | +6.8 ms | 同上 |

首帧 2.3 s 主要是等待自然关键帧（fixture GOP 2 s，无 GOP cache），HLS 首帧 0.09 s 指首个片段内的解码起点（片段生成延迟另计）。HLS `stall` 计数为片段式到达，属协议特性。RTSP 推流的 AAC 音频 gap 来自 `third/` 内 avpbs-aac 的整数毫秒时间戳，未修改 `third/`，见 §10。

## 4. Phase 2：A1.1 TCP 背压

### 4.1 F5 根因

`tcp_transport::on_write` 每次完成只发出队首一条消息，下一条要等完成回调被重新排到事件循环队尾。CPU 饱和时，每个 viewer 每轮事件循环只能发 1 条，而同一轮交付到它的帧有多条，写队列单调增长直至 1 MiB 上限触发断开。RTMP 9000：9000 × 1 MiB 加分配器开销约 14 GiB PSS，约 1100–2200 个 viewer 被断开（EOF）。

否定的假设：

- 内核发送阻塞：`ss` 显示 `tcp_tx_queue ≈ 0`，积压在用户态队列。
- 已投递未执行的写（post 堆积）是主因：仅把 `post` 改为 `dispatch`（`rt-a11`）后 RTMP 9000 仍为 PSS 14.4 GiB、2208 个 EOF。

本地单元测试 `tests/tcp_transport_test.cpp` 的 `completion_lap_backlog` 场景复现该机制（每 200 µs 事件循环一轮写 3 条 4000 B，读端足够快）：修改前 3/3 RED（写出 528000 字节后 `ENOBUFS`），修改后 3/3 GREEN。

### 4.2 修改

- `tcp_transport`：写完成后把队列中全部积压作为一组 `const_buffer` 一次 `async_write`；完成时按批量出队。顺序、1 MiB 上限、错误回调一次、shutdown 后不回调等语义不变，没有新增状态、锁或全局队列。`write()` 改为 `dispatch`：在 owner worker 上直接入队并立即按上限检查，省去每次写的 post。
- `http_flv_session`：输出队列同样在写完成后合并为一个 chunk（`make_chunk(vector<const_buffer>)`），4 MiB 上限不变。

### 4.3 结果

| 负载 | A（`rt-cfe80a0`） | 仅 dispatch | G（合并） | GD（合并 + dispatch） |
| --- | --- | --- | --- | --- |
| RTMP 9000 | 3/3 FAIL：PSS 14.0–15.1 GiB，实时比 0.967–0.975，滞后 max 3.7–3.8 s | FAIL：PSS 14.4 GiB | 2/2 PASS：PSS 3.3 GiB，7.94 核 | 2/2 PASS：PSS 2.9–3.1 GiB，7.44 核，实时比 0.998，滞后 max 264–300 ms |
| RTSP 8500 | FAIL：PSS 12.2 GiB，实时比 0.970 | FAIL：PSS 14.4 GiB | PASS：PSS 3.1 GiB，7.92 核 | PASS：PSS 2.9 GiB，7.61 核 |
| RTMP 6500 周期（perf stat） | 1008–1010 G（6.19 核） | — | 1032–1035 G（+2.6%） | 893 G（−11.4%，5.60 核） |

| HTTP-FLV | A | GDF（`46f0552`） |
| --- | --- | --- |
| FLV 8500 | r1 BLOCKED（PSS 超 16 GiB 预算）；r2 FAIL：PSS 10.3 GiB，实时比 0.951，滞后 max 2980 ms | 2/2 PASS：PSS 2.2–2.4 GiB，实时比 0.996–0.997，滞后 max 328–347 ms |
| FLV 6500 | 6.35 核，2.64 Mcyc/viewer | 6.13–6.17 核，2.56–2.57 Mcyc/viewer（−2.5…−3.3%） |

系统调用（bpftrace，RTMP 8500）：HEAD 649 k sendmsg/s × 3434 B，214 G 周期，7.88 核；GD 574 k/s × 3941 B，196 G 周期，7.23 核。

G 单独合并写出比 A 多 2.6% 周期（每次完成构造 buffer 向量），dispatch 把它变为净减 11%：同 worker 写不再经过事件循环往返。

### 4.4 慢客户端、停止读取与生命周期

| 场景 | 结果 |
| --- | --- |
| 1000 RTMP + 1000 RTSP + 500 GB passive + 每协议 20 个 SIGSTOP 读者，60 s 与 120 s（A、仅 dispatch、GDF 交替） | 三版本一致：健康 viewer 实时比 ≈1.0，控制 p99 22–56 ms；慢读者不拖累健康 viewer |
| 慢读者是否被关闭（TEST） | 120 s 内 0/40 被关闭：TEST netns 的 `tcp_wmem`/`tcp_rmem` 上限 2 GiB，停止的读者在 120 s 内吸收全部数据，应用层 1 MiB 上限未触达。属环境特性 |
| 慢读者是否被关闭（本地，默认 TCP 缓冲） | `pf-{tsan,asan}-GD-stall` 与 `pf-{tsan,asan}-A3-stall`：每轮 RTMP/RTSP/FLV 各 3 个停止读者全部被服务端关闭（9/9），健康 viewer 70/70 progressing |
| 本地 sanitizer（GD+FLV） | TSan/ASan/UBSan 生命周期场景 0 诊断；`mixed` 80/80、`src4`（含 churn）140/140、`stall` 70/70 PASS；ASan 报告仅 makecontext 提示 |
| transport 单元测试（G、GD） | 普通、TSan、ASan+UBSan 全部通过：顺序与完整性（owner 线程与外部线程写）、溢出只报告一次、在途 shutdown、对端 reset、completion lap |

## 5. Phase 3：A1.2 GB TCP 合并写出

三个版本：A2 之前（`rt-a2A-c999003`）、A2（`rt-cfe80a0`，20 ms 切片）、A2 + A1.1（`rt-a11gdf`）。GB TCP 每个 sender 的输出经 PS muxer 与 `tcp_transport`，A1.1 已对其生效，因此 A1.2 要回答的是：A2 的 20 ms 切片是否削弱了 GB 的批量写出，是否需要单独让合并与 drain 长度无关。

系统调用批量（bpftrace `wb.sh`）：

| 负载 | A2 之前 | A2 | A2 + A1.1 |
| --- | --- | --- | --- |
| GB TCP active 12000 | 266.6 k send/s × 12.4 KB，217 G 周期 | 276.8 k × 11.8 KB，216.5 G | 354 k × 9.3 KB，214 G，指令 +10% |
| GB TCP passive 9500 | 6290 B/send | 6388 B/send | — |
| GB TCP active 6000 | 5280 B/send | 5136 B/send | — |

A2 切片前后每次发送字节数变化 < 5%，批量主要由 PS 打包决定，与 drain 长度无关。A1.1 让每次写更早发出、批量略小、调用更多，但周期不增。

容量与控制（每版本 1 轮，交替执行；create 为每 256 个请求一组的组内分位，表中给出组间中位数 / 最差组；客户端只输出 p50/p90/p99，没有 p95）：

| 负载 | 版本 | 状态 | 核 | PSS MiB | 实时比 | 滞后 max ms | create p50 | create p90 | create p99 | 首帧 p50 / 最差组 p99 ms |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| active 9500 | pre | PASS | 7.55 | 4070 | 0.998 | 280 | 4 / 115 | 20 / 400 | 94 / 424 | 1100 / 2283 |
| | A2 | PASS | 7.53 | 4119 | 0.997 | 312 | 4 / 360 | 22 / 458 | 98 / 487 | 1052 / 2294 |
| | A1.1 | PASS | 7.22 | 3868 | 0.998 | 348 | 4 / 41 | 19 / 282 | 85 / 319 | 1046 / 2242 |
| active 10000 | pre | PASS | 7.69 | 4290 | 0.998 | 360 | 4 / 158 | 24 / 496 | 94 / 558 | 1032 / 2269 |
| | A2 | PASS | 7.70 | 4332 | 0.998 | 325 | 4 / 169 | 20 / 562 | 92 / 629 | 1058 / 2309 |
| | A1.1 | PASS | 7.53 | 3564 | 0.999 | 260 | 4 / 178 | 20 / 483 | 88 / 520 | 1028 / 2334 |
| active 12000 | pre | PASS | 7.93 | 5250 | 0.997 | 406 | 6 / 1185 | 34 / 2240 | 114 / 2466 | 1122 / 3573 |
| | A2 | PASS | 7.95 | 5249 | 0.999 | 407 | 6 / 2057 | 31 / 2374 | 114 / 2447 | 1141 / 3739 |
| | A1.1 | PASS | 7.88 | 5247 | 0.998 | 451 | 6 / 944 | 30 / 1095 | 100 / 1131 | 1095 / 3361 |
| active 13000 | pre | PASS | 7.92 | 5800 | 0.997 | 450 | 9 / 1890 | 35 / 2351 | 117 / 2423 | 1204 / 4217 |
| | A2 | PASS | 7.94 | 5728 | 0.999 | 449 | 8 / 4627 | 31 / 5068 | 118 / 5353 | 1218 / 6770 |
| | A1.1 | PASS | 7.87 | 5591 | 0.997 | 392 | 7 / 1651 | 30 / 1969 | 109 / 2030 | 1208 / 3698 |
| passive 9500 | pre | PASS | 7.52 | 3968 | 0.997 | 268 | 4 / 60 | 22 / 388 | 98 / 440 | 1080 / 2259 |
| | A2 | PASS | 7.56 | 4005 | 0.997 | 277 | 4 / 33 | 20 / 344 | 85 / 405 | 1042 / 2259 |
| | A1.1 | PASS | 7.33 | 3375 | 0.999 | 297 | 4 / 286 | 18 / 303 | 88 / 329 | 1056 / 2267 |
| passive 10000 | pre | PASS | 7.73 | 4257 | 0.998 | 290 | 4 / 321 | 23 / 551 | 90 / 615 | 1069 / 2285 |
| | A2 | PASS | 7.72 | 4332 | 0.998 | 425 | 4 / 171 | 20 / 552 | 94 / 610 | 1076 / 2377 |
| | A1.1 | PASS | 7.51 | 4012 | 0.999 | 290 | 4 / 88 | 20 / 413 | 82 / 470 | 1071 / 2330 |
| passive 12000 | pre | PASS | 7.94 | 5186 | 0.998 | 408 | 7 / 937 | 29 / 1350 | 102 / 1407 | 1091 / 2904 |
| | A2 | PASS | 7.93 | 5280 | 0.998 | 406 | 6 / 3078 | 24 / 3398 | 105 / 3461 | 1134 / 4765 |
| | A1.1 | PASS | 7.90 | 5056 | 0.998 | 373 | 6 / 1150 | 27 / 1662 | 113 / 1768 | 1175 / 3548 |
| passive 13000 | pre | PASS | 7.91 | 5830 | 0.996 | 523 | 7 / 3797 | 33 / 4309 | 124 / 4389 | 1317 / 5618 |
| | A2 | PASS | 7.92 | 5847 | 0.996 | 475 | 7 / 1354 | 34 / 4153 | 120 / 5003 | 1214 / 6403 |
| | A1.1 | PASS | 7.92 | 5411 | 0.997 | 373 | 7 / 1438 | 36 / 1637 | 114 / 1706 | 1206 / 2983 |

分析：

- 9500–10000：三版本全部 PASS，组间 create 中位数相同（4 ms），A1.1 少用 0.15–0.3 核、PSS 低 4–18%。
- 12000–13000：服务端 7.9 核饱和，实时比仍 ≥ 0.996（媒体持续交付），差别集中在建链期间最差组的尾延迟：它们发生在 ramp 时与已建立会话争抢 CPU 的那几组，单轮差异较大（A2 在 active 13000、passive 12000 最差，pre 在 passive 13000 最差）。A1.1 在 4 个饱和负载下最差组 p99 均为最低或接近最低（1.1–2.0 s）。
- A2 切片对 GB TCP 批量和 create 中位数没有可测影响；A1.1 之后不存在“合并依赖 drain 长度”的独立瓶颈。

结论：A1.2 REJECT，不增加 GB TCP 专用的合并逻辑。

## 6. Phase 4：A3 UDP 接收缓冲

### 6.1 区分丢包来源

| 来源 | 判别证据 | 观测 |
| --- | --- | --- |
| 内核接收队列溢出 | `/proc/net/snmp` `RcvbufErrors`（`udp_drops`）、`ss -uamp` 的 `d` 计数 | 中等负载下有 |
| 网络丢包 | 客户端与服务端在同机 netns 间经 veth 通信，没有物理链路；`RcvbufErrors` 为 0 的轮次没有媒体缺失 | 未观测到 |
| 用户态处理慢 / 事件循环延迟 | `ss` Recv-Q 持续接近 `rb`，且单核 busy ≈ 1.0 | 只在单 worker 饱和时出现（WHIP 200 r1、WHIP 250） |
| 重组失败 | `RcvbufErrors` 为 0 时仍出现 viewer 停滞或实时比下降 | 未观测到：0 丢包轮次实时比均 ≥ 0.999 |

`a3.sh` 在测量窗口第 20 s 抓取 `ss -uanmp`。GB UDP 300 默认缓冲失败轮次中所有接收 socket 的 Recv-Q ≈ 0、单核 busy 0.35，但 `udp_drops` 139：用户态平时跟得上，丢包发生在瞬时突发（多路同时到达关键帧分片）超过 `rb212992` 时。

### 6.2 SO_RCVBUF 语义与内存

- 内核把 `setsockopt(SO_RCVBUF, n)` 记为 `2 × min(n, rmem_max)`；256 KiB → `rb524288`（`ss` 实测）。`rmem_max` 为常见默认 212992 时生效值为 416 KiB。
- 接收缓冲按实际排队字节计费，不预分配：PSS 在 A/B 间无差别（GB UDP 300：299–303 MiB；WHEP 100：231–235 MiB）。最坏情况是每个满队列 socket 占 512 KiB 内核内存，1000 路输入约 512 MiB。
- `rmem_default`/`rmem_max` 在 6.8 内核不按 netns 隔离，修改会影响 TEST 主机上的其他服务，因此没有做 sysctl 实验，改为进程内对特定 socket 设置。

### 6.3 实验与范围选择

实验版本：S = 所有 UDP socket 256 KiB（`rt-a3s`），L = 所有 UDP socket 1 MiB（`rt-a3l`）。

| 负载 | 默认（A/F） | S 256 KiB | L 1 MiB |
| --- | --- | --- | --- |
| GB UDP 输入 300 → FLV | A FAIL：drops 139，滞后 max 538 ms | PASS：0，72 ms | PASS：0，72 ms |
| WHIP 200 → FLV | A FAIL：drops 6888，滞后 max 2072 ms | PASS：0，173 ms | PASS：0，123 ms |
| WHEP 拉流 100 → FLV | A FAIL：drops 737，滞后 max 591 ms | PASS：0，84 ms | PASS：0，75 ms |
| H.265 GB（低负载） | 2/2 PASS | 2/2 PASS | — |

1 MiB 相对 256 KiB 没有进一步收益，选 256 KiB。全 socket 生效会把 GB UDP sender（数万个输出 socket，从不读取，对端 RTCP 会填满接收队列）的内核内存放大一倍以上，因此只对接收媒体的 RTP socket 生效：GB UDP 接收、RTSP UDP 推流 RTP、WHIP、WHEP 拉流；输出与 RTCP socket 保持系统默认。

### 6.4 FINAL 确认（`rt-a3f` 对 `rt-a11gdf`，交替）

| 负载 | F（默认） | A3 |
| --- | --- | --- |
| GB UDP 300 | INCONCLUSIVE：drops 83，滞后 max 538 ms | PASS：0，64 ms；`ss`：300 × `rb524288`（RTP），其余 `rb212992` |
| WHEP 拉流 100 | FAIL：drops 840，滞后 max 585 ms | PASS：0，76 ms |
| WHEP 拉流 150 | — | PASS：0，6.83 核，132 ms |
| GB UDP 500 | — | 0 drops，71 ms（客户端资源 INCONCLUSIVE） |
| RTSP UDP 推流 300 | PASS：0 | PASS：0；600 × `rb524288`（2 轨 RTP），600 × 默认（RTCP） |
| WHIP 200 | 3/3 PASS：0 drops，最高单核 0.79–0.81 | r1 FAIL：drops 230667，单核 1.000；r2、r3 PASS：0 drops，单核 0.80–0.86 |
| WHIP 250 | — | FAIL：drops 536177，单核 1.000，7.28 核（容量边界） |
| GB UDP 输出 7500 | — | PASS：15000 个 socket 全部 `rb212992` |

WHIP 200 的 A3 r1：55 个 socket 的 Recv-Q 达 270–450 KB，测量期内有一个核 busy = 1.000。这是单个 worker 饱和后停止及时读取，属于用户态处理能力不足，加大缓冲只会加深队列，不能消除丢包。WHIP 200 在默认缓冲下也仅 1/4 轮出现突发溢出（`a3-whip200-A`）。

结论：

- GB UDP 输入、WHEP 拉流：默认缓冲 4/4 轮突发溢出、出现 0.5 s 以上卡顿；256 KiB 后 6/6 轮 0 丢包。ACCEPT。
- WHIP：200 路时既有突发溢出（默认 1/4 轮）也有 worker 饱和（A3 1/3 轮），两种失败各自出现，样本不足以区分收益，INCONCLUSIVE；保留设置，成本只在队列真正积压时产生。
- RTSP UDP 推流：300 路时两版本均无丢包，INCONCLUSIVE（无害）。

部署说明见 `docs/build-deploy.md` 的“UDP 媒体接收缓冲”一行。

### 6.5 sanitizer

A3 构建（`46f0552` + 未提交的 A3 改动，与 `28b8ede` 源码相同）本地 TSan/ASan/UBSan：生命周期场景 0 诊断；`mixed` 80/80、`src4` 140/140、`stall` 70/70 PASS（TSan、ASan）；ASan 报告仅 makecontext 提示。sanitizer ELF：TSan `c90c72c0…`、ASan `2ce10f6a…`、UBSan `38bd4397…`。

## 7. 各协议资源与容量（BASE 对 FINAL）

### 7.1 三类容量的定义

- **最大连接保持容量**：所有 viewer 连接建立，测量窗口内服务端没有主动断开，客户端仍在收到字节（progressing）。不要求实时。
- **最大持续媒体交付容量**：在连接保持的基础上，所有 viewer 实时比 ≥ 0.98、无服务端丢包证据、PSS 在 16 GiB 预算内（即测试判定 PASS）。服务端可以已经饱和。
- **满足实时监控体验要求的稳定容量**：持续交付的基础上留出余量：服务端 ≤ 7.2 核（8 核的 90%，给 churn、关键帧突发和控制面留空间），实时比 ≥ 0.995，滞后 max ≤ 300 ms，控制请求 p99 ≤ 200 ms，首帧与音画同步不劣于低负载基线。

单源 RTMP 推流、720p30 fixture，每协议单独测量；混合负载为 RTMP/FLV/RTSP/WHEP/GB UDP/GB TCP passive 六种输出等分。

### 7.2 容量结果

| 输出 | 版本 | 稳定容量（≤ 7.2 核） | 最大持续交付 | 最大连接保持（已测） |
| --- | --- | --- | --- | --- |
| RTMP | BASE | ≥ 7500（7.17 核，实时比 0.998，滞后 max 238 ms） | 7500 ≤ x < 8500（8500 FAIL：实时比 0.962，1062 个被断开，PSS 13.5 GiB） | ≥ 7500，< 8500：8500 时 1062 个、9000 时 2182–2217 个被断开；10000 建链期 PSS 超 16 GiB（BLOCKED） |
| | FINAL | ≥ 8000（6.97 核，0.997，235 ms） | 12000 稳定（0.998，342 ms）；14000 临界（0.984，1262 ms） | ≥ 14000 |
| RTSP | BASE | ≥ 7000（7.11 核，0.999，244 ms） | 7000 ≤ x < 8000（8000：8000 个全部 progressing，但实时比 0.955、滞后 2.8 s） | ≥ 8000；8500 时 2126 个被断开 |
| | FINAL | ≥ 7500（7.12 核，0.999，195 ms） | 11500 ≤ x < 13000（13000：全部 progressing，实时比 0.918） | ≥ 13000 |
| HTTP-FLV | BASE | ≥ 7000（6.91 核，0.999，222 ms） | 8000 ≤ x < 8500（8500：全部 progressing，实时比 0.951、滞后 3 s；另一轮 PSS 超 16 GiB） | ≥ 8500（PSS 10.3 GiB 至超出预算） |
| | FINAL | ≥ 7500（7.09 核，0.997，244 ms） | ≥ 13000（实时比 1.0，滞后 582 ms，控制 p99 394 ms） | ≥ 13000 |
| 混合 6 协议 | BASE | ≥ 6600（6.87 核，0.999，318 ms） | 7500 ≤ x < 8400（A 端 `rt-cfe80a0`：8400/9000 建链期被断开，PSS 5.8–6.7 GiB） | < 8400 |
| | FINAL | ≥ 6600（6.75 核，0.999，259 ms） | ≥ 9000（0.999，378 ms，PSS 2.5 GiB） | ≥ 9000 |
| GB TCP active/passive | A2 / FINAL | 约 9000（9500 时 7.2–7.5 核） | ≥ 13000（三版本都 PASS） | ≥ 13000 |
| WHEP（输出） | A / FINAL | — | 3500 + churn 50 两版本都 PASS（7.47–7.49 核） | — |
| GB UDP（输出） | A / FINAL | — | 7500 两版本都 PASS（7.05–7.08 核） | — |
| 多源 RTMP 100 源 × 9000 | FINAL | — | PASS（7.97 核，0.999，滞后 max 127 ms） | — |

说明：

- BASE 的 RTMP/RTSP/混合数据部分来自 `rt-cfe80a0`（BASE + F1/F2 + 工具），它与 BASE 的 TCP 写路径代码相同；同日 `rt-p1-base` 的 RTMP 8500、RTSP 8000、RTMP 10000 结果一致。
- “稳定容量”是已测且满足条件的最高点（≥），未逐级细扫；FINAL 在相同 CPU 余量下大约多 500 路（RTMP/RTSP/FLV 约 +7%）。
- 持续交付容量：RTMP 由 < 8500 到 ≥ 12000，RTSP 由 < 8000 到 ≥ 11500，FLV 由 < 8500 到 ≥ 13000，混合由 < 8400 到 ≥ 9000。提升来自过载行为的改变：饱和后不再因写队列积压而内存失控、断开，而是降低每帧的系统调用和周期，继续实时交付。这些负载下服务端已占满 8 核，控制 p99 270–390 ms、滞后 300–600 ms，不应作为常态运行点。
- FINAL 的 RTSP 13000 是“连接保持、在收字节，但不是实时观看”的典型例子；BASE RTSP 8000、FLV 8500 同理。

### 7.3 每 viewer 成本

| 负载 | BASE | FINAL | 变化 |
| --- | --- | --- | --- |
| RTMP 6500（同日哨兵） | 6.35 核，1033 G 周期，2.64 Mcyc/viewer，PSS 2156 MiB | 5.69 核，915 G 周期，2.34 Mcyc/viewer，PSS 2038 MiB | 周期 −11.5% |
| RTMP 6500（A1.1 期间，各 2 轮） | 6.19 核，1008–1010 G | 5.58–5.61 核，893 G | −11.4% |
| FLV 6500（各 2 轮） | 6.34–6.35 核，2.64 Mcyc/viewer | 6.13–6.17 核，2.56–2.57 | −2.5…−3.3% |
| 混合 6600 | 6.87 核，2.81 Mcyc/viewer，PSS 1737 MiB | 6.75 核，2.76，PSS 1598 MiB | −1.7% |
| 混合 6005 + churn（各 2 轮） | 6.60–6.66 核，PSS 1238 MiB | 6.42–6.58 核，PSS 1201–1211 MiB | −2% |
| GB TCP active 9500 | 7.53 核（A2） | 7.22 核 | −4% |

每 viewer 周期随负载上升而下降（RTMP 6500 → 14000：2.34 → 1.54 Mcyc/viewer），因为饱和后每次写出合并了更多帧。

## 8. 用户体验

### 8.1 低负载（单源，每种输入 × 7 种输出，各 1 轮，BASE 与 FINAL 交替）

| 输入 | 版本 | 首帧 RTMP/FLV/RTSP/HLS (s) | 帧率 | 视频间隔 max | 音画偏差 min/p50/max (ms) | 滞后 max | 控制 p99 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| RTMP | BASE | 2.29/2.28/1.86/0.11 | 30.0 | 34 ms | 1.5/1.5/1.5 | 47 ms | 3.7 ms |
| | FINAL | 2.24/2.24/1.80/0.11 | 30.0 | 34 ms | 1.5/1.5/1.5 | 50 ms | 3.7 ms |
| RTSP TCP | BASE | 2.28/2.27/1.83/0.09 | 30.0 | 35 ms | 3.5/8.3/13.2 | 49 ms | 2.7 ms |
| | FINAL | 2.26/2.24/1.83/0.09 | 30.0 | 34 ms | 2.0/6.8/12.2 | 59 ms | 2.8 ms |
| WHIP | BASE | 2.32/2.31/1.90/0.11 | 30.0 | 34 ms | −24.8 恒定 | 26 ms | 3.9 ms |
| | FINAL | 2.30/2.29/1.88/0.11 | 30.0 | 34 ms | −24.8 恒定 | 27 ms | 3.8 ms |
| WHEP 拉流 | BASE | 2.31/2.29/1.88/0.11 | 30.01 | 46 ms | −56.5/−36.0/−13.2 | 64 ms | 3.3 ms |
| | FINAL | 2.30/2.29/1.87/0.10 | 30.0 | 34 ms | −56.0 恒定 | 48 ms | 3.7 ms |
| GB UDP | BASE | 2.31/2.30/1.91/0.10 | 30.0 | 34 ms | 2.8 恒定 | 52 ms | 3.7 ms |
| | FINAL | 2.31/2.30/1.91/0.10 | 30.0 | 34 ms | 2.8 恒定 | 52 ms | 4.0 ms |
| GB TCP active | BASE | 2.23/2.21/1.81/0.11 | 30.0 | 34 ms | 2.8 恒定 | 53 ms | 2.9 ms |
| | FINAL | 2.23/2.24/1.81/0.10 | 30.0 | 34 ms | 2.8 恒定 | 53 ms | 3.7 ms |

所有组合视频无 pts 回退、无到达停顿（HLS 的 16–18 次为片段式到达），解码告警 0。

### 8.2 负载下（6005 路混合 + 50 路 RTMP churn，A/V 探测 40 s，各 2 轮交替）

| 版本 | 核 | PSS | 实时比 | 滞后 p50/max | 控制 p50/p99 | churn | 首帧 RTMP/FLV/RTSP/HLS | 音画偏差 | 视频间隔 max |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| BASE | 6.60–6.66 | 1238 MiB | 0.9998–0.9999 | 79–82 / 84–94 ms | 12.1–12.3 / 34.8–45.5 ms | 2/2 | 3.17–3.20 / 3.06–3.13 / 2.57–2.62 / 0.17–0.18 s | 0.0（RTSP/HLS ±0.4） | 34 ms |
| FINAL | 6.42–6.58 | 1201–1211 MiB | 0.9999 | 60–66 / 70–73 ms | 10.1–11.5 / 31.2–32.7 ms | 2/2 | 3.20–3.22 / 3.11–3.14 / 2.61 / 0.15–0.16 s | 同左 | 34 ms |

这个负载在两版本的稳定区内（< 7.2 核）：体验相当，FINAL 滞后和控制尾延迟略低。负载下首帧比低负载多约 0.9 s，主要是建链排队加上等待下一个关键帧。

### 8.3 过载下的体验差异

BASE 饱和后滞后升至 2–4 s、实时比 0.95–0.97，并有约 1/4 viewer 被断开；FINAL 在 RTMP 9000–12000、FLV 9000–13000、RTSP 9000–11500 下实时比 ≥ 0.994，滞后 max 230–580 ms。

## 9. 控制面、过载与生命周期

### 9.1 控制响应（`GET /receivers`，每 200 ms 一次）

| 负载 | BASE / A | FINAL |
| --- | --- | --- |
| 低负载 | p99 2.7–3.9 ms | p99 2.8–4.0 ms |
| 混合 6005 + churn | p99 35–46 ms | p99 31–33 ms |
| 稳定区（RTMP 7500/8000、RTSP 7000/7500、FLV 7000/7500） | p99 79–143 ms | p99 92–129 ms |
| RTMP 9000（A 已失败 / FINAL 通过） | p99 96–111 ms（同时有 viewer 被断开） | p99 132–216 ms |
| 饱和（RTMP 12000、FLV 13000、GB TCP 13000） | — | p99 295–394 ms，无错误、无 > 1 s 请求 |

饱和时控制尾延迟随负载上升，是 8 核占满后的排队；已测最高负载下仍 < 0.4 s。

### 9.2 GB sender 建链

见 §5：create 组间中位数 4–9 ms，三版本一致；饱和（12000–13000）时最差组 p99 为 1.1–5.4 s，A1.1 版本 1.1–2.0 s。

### 9.3 慢客户端与停止读取

- TEST 60/120 s（1000 RTMP + 1000 RTSP + [1000 FLV] + 500 GB passive + 每协议 20 个 SIGSTOP 读者）：A、仅 dispatch、GDF、FINAL 均 PASS，健康 viewer 实时比 ≈ 1.0、控制 p99 22–56 ms。慢读者不影响其他 viewer，但因 TEST netns TCP 缓冲上限 2 GiB，60/120 s 内未触发应用层 1 MiB 上限，0 个被关闭。BASE bundle 的客户端没有 `--stalled-viewers`，对照使用 TCP 写路径相同的 `rt-cfe80a0`。
- 本地默认 TCP 缓冲（sanitizer 构建）：停止读者 9/9 被关闭，健康 viewer 70/70 progressing。

### 9.4 源结束与批量关闭

`--fault source_stop`（4 源，RTMP/RTSP/FLV 各 500、GB passive 250，测量中途 kill 发布端）：BASE 与 FINAL 的 7 组 viewer 都以 `SOURCE_ENDED`（EOF）结束，判定 FAIL 是该负向场景的预期结果；清理后 fd 回到 32、线程 16。

### 9.5 资源释放

所有 run 清理后 fd 回到 32、线程回到 16（GB-only 9），无泄漏迹象。PSS 不回落：FINAL 大负载后保留 1.3–5.0 GiB（BASE 同样如此，RTMP 6500 清理后 BASE 1907 MiB、FINAL 1879 MiB），是分配器保留的空闲页，不随版本变化，见 §10。

### 9.6 并发与 sanitizer

见 §4.4 与 §6.5：A1.1 与 A3 构建在 TSan、ASan、UBSan 下生命周期与负载场景 0 诊断。transport 单元测试覆盖 owner 线程与外部线程交错写、在途 shutdown、对端 reset 与溢出。

## 10. 遗留问题

| 问题 | 现象与依据 | 建议 |
| --- | --- | --- |
| 转码引起的恒定音画偏差 | WHIP −24.8 ms、WHEP 拉流 −56 ms，恒定不漂移 | 在 AAC 编码输出上补偿编码器延迟（priming），单独评估 |
| RTSP 推流 AAC 音频 pts gap | 每 40 s 约 56 次 ≤ 10 ms gap，无丢帧；来自 `third/` avpbs-aac 整数毫秒时间戳 | 需要修改 `third/`，本轮未授权 |
| GB UDP sender 不读取 socket | 对端 RTCP 填满每个输出 socket 的接收队列至 `rmem_default`；数万 sender 时占内核内存 | 读取并丢弃，或关闭接收方向；需单独设计 |
| WHIP 单 worker 饱和 | WHIP 200 时 1/3 轮、250 时出现单核 1.000，该 worker 上的 socket 被填满 | 属 worker 负载不均（WHIP 转码集中），A3 不能解决 |
| 清理后 PSS 不回落 | 大负载结束后保留 1.3–5.0 GiB，BASE 同样存在 | 评估 jemalloc `dirty_decay_ms`/`muzzy_decay_ms` 或后台回收 |
| av_probe 只检查 pts | 无法发现 F1 那种 dts 半速问题（该问题由性能客户端的实时比发现） | 在 av_probe 中增加 dts 单调性与速率检查 |
| TEST netns TCP 缓冲过大 | `tcp_wmem`/`tcp_rmem` 上限 2 GiB，停止读者在 120 s 内不触发应用层上限 | 慢客户端关闭需在默认缓冲环境或更长窗口下验证 |
| F3/F4/F6 | 本轮未处理 | 维持 8 核报告结论 |
| GB create 客户端统计 | `gb_fanout` 只按组输出 p50/p90/p99，没有逐请求数据，无法给出 p95 | 需要时让客户端输出逐请求耗时 |

## 11. 提交与验证清单

| 提交 | 内容 | 验证 |
| --- | --- | --- |
| `3c6fddc` HLS 跳过 G.711 音轨 | `hls_segmenter.cpp`、`docs/protocols.md` | §3.1 |
| `aa9b449` 性能测试增加音画时间线与慢读者探测 | `bench/av_probe.py`、`bench/perf_run.py` | 全部 UX 与慢读者 run 使用 |
| `cfe80a0` WebRTC 与 RTSP 推流只用首个 SR 对齐时间线 | `whip_media_receiver`、`rtsp_publish_media` | §3.2，B 帧回归 |
| `9a1d603` TCP 写完成后合并写出积压 | `tcp_transport`、`docs/media-lifecycle.md` | §4，单元测试 RED→GREEN，sanitizer |
| `46f0552` HTTP-FLV 写完成后合并写出积压 | `http_flv_session.cpp`、`docs/media-lifecycle.md` | §4.3；按此提交重建 ELF 与测试 bundle SHA 相同 |
| `28b8ede` UDP 媒体输入 socket 加大接收缓冲 | `udp_transport` 与 7 个调用点、`docs/build-deploy.md` | §6；FINAL ELF 由该树构建 |

未执行：完整 CTest（仓库已移除第一方测试注册）、Go 信令与 Qt 客户端构建（本轮未改动）。未 push。
