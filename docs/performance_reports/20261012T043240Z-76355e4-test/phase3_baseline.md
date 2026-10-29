# 阶段 3A：严格资源基线

运行：`20261012T043240Z-76355e4` · TEST `<TEST_HOST>` · 网络命名空间 `msperf-20261012T043240Z-76355e4`
被测：`media_server` HEAD `76355e435a766ed6d6c58be5dc315d14256b4e65`，构建产物 SHA256 `e8f8e298…`（阶段 1 已验证）
采样脚本：`pi_phase32b.py`（DUT 扩展指标）、`pi_phase32c.py`（publisher / signaling 独立成本）
原始数据：TEST `/var/tmp/media_server_perf/20261012T043240Z-76355e4/phase3-baseline/`

采样方法：1 Hz 读取 `/proc/<pid>/{stat,smaps_rollup,status,fd,net/netstat,net/sockstat}` 与 `/proc/<pid>/task/*/{stat,io}`；
CPU 核数 = 窗口内 utime+stime+cutime+cstime 增量 / 墙钟；PSI 读 `/proc/pressure/{cpu,memory,io}` 与 `/proc/<pid>/pressure`。
未修改全局 sysctl / ulimit / cgroup；DUT 固定 `taskset 0-7`（8 物理核，`--threads 8`），辅助进程 `taskset 8-15`。

## A1 空闲基线（只有 DUT，无 source、无 viewer）

3 次独立启动，每次 30 s 稳定窗口，结果完全一致：

| 指标 | 中位数 | 最大值 |
| --- | --- | --- |
| CPU | 0.00000 核 | 0.00000 核 |
| PSS | 15.9 MiB | 15.9 MiB |
| RSS | 17.9 MiB | 17.9 MiB |
| FD | 26 | 26 |
| 线程 | 8 | 8 |
| TCP established（11935/18080/18554） | 0 / 0 / 0 | — |
| ESTABLISHED 套接字（DUT netns） | 0 | 0 |
| TCP 活跃连接（netstat） | 1（监听） | 1 |
| 线程上下文切换增量 | 0 | 0 |
| 线程 voluntary 切换增量 | 0 | 0 |
| 线程等待 CPU 时间 | 0 ms | 0 ms |
| 系统 PSI cpu / memory / io（some avg60） | 0.00 / 0.00 / 0.00 | 同 |
| DUT 进程 PSI cpu some avg60 | 0.00 | 0.00 |

结论：空闲态无后台忙等、无周期任务、无泄漏型 FD/线程占用。后续所有增量以此为 0 点。

## A2 严格 publish-only（1 source，0 viewer）

3 次独立启动（signaling + DUT + `perf_publish` 1 source RTMP，`cycles=1`），每次 generation ready 后静置 8 s，再取 30 s 窗口。

0 viewer 的判定证据（三次一致）：

- `ss` 检查：11935 ESTABLISHED = 1（唯一发布连接）、18080 = 0、18554 = 0；
- 控制面 `/receivers` 返回 1 个 ingest `stream_id`、`/gb28181/sender` 返回 `senders=0`。控制面只列出输入会话，不提供 viewer 计数，因此 viewer=0 只由上一条 `ss` 证据确定；
- 发布进程在整个窗口内保持存活（`publisher_alive_after_window=true`）。

DUT（30 s 窗口）：

| 指标 | 结果 | 相对空闲增量 |
| --- | --- | --- |
| CPU（窗口首末点差值） | **0.0062 核**（阶段 4A 同配置复测，12 次运行 0.0058–0.0062） | **+0.0062 核** |
| ~~原始 1 Hz 采样中位数~~ | ~~0.00892 – 0.00896 核~~（估计量有偏，见下方修正说明，已废弃） | — |
| PSS 中位数 | 19.9 MiB | **+4.0 MiB** |
| PSS 最大值 | 20.0 MiB | +4.1 MiB |
| RSS 中位数 | 22.8 MiB | +4.9 MiB |
| FD | 33 | +7 |
| 线程 | 10 | +2 |
| 单线程最大 CPU | ≈0.006 核 | 集中在 1 个 I/O 线程 |
| 上下文切换增量 | 2 900 – 2 903 / 30 s | ≈ 97/s（≈ 视频 30 + 音频 47 + RTCP/keepalive） |
| 等待 CPU 时间 | 1 – 2 ms | 可忽略 |
| PSI（系统/进程） | 0.00 | 无资源压力 |

publisher（ffmpeg，独立进程，3 次）：

| 指标 | 结果 |
| --- | --- |
| CPU | 0.0113 核（阶段 4A 用窗口差值复测；本工具原报的 ≈0.01 核是 1 Hz 采样下限） |
| PSS | 26.3 – 26.9 MiB |
| RSS | 29.5 – 30.1 MiB |
| FD / 线程 | 5 / 3 |

signaling（真实 Go 服务，独立进程，3 次）：

| 指标 | 结果 |
| --- | --- |
| CPU | 0.0 核（窗口内无请求，仅心跳/无活动） |
| PSS | 13.7 – 13.8 MiB |
| RSS | 15.4 – 15.5 MiB |
| FD / 线程 | 8 / 8–9 |

采样修正（阶段 4 发现，已回改本表）：本项最初记录 0.0089 核，来自 1 Hz 相邻采样差值的中位数。该进程每秒 CPU 不足 1 个时钟节拍（1 jiffy = 0.00994 核·秒），而实际采样间隔约 1.11 s，使每个区间都恰好计入 1 拍并除以 1.11 s，系统性高估约 1.4 倍。改用窗口首末点差值（总 CPU 时间 / 墙钟）后为 0.0062 核，阶段 4A 以同配置 12 次运行复现。`perf_run` 的服务端指标始终使用首末点差值，因此阶段 3B 协议矩阵数据不受此影响。

结论：

1. 1 路 RTMP 接入（30 fps H.264 + AAC，1.53 Mbps）在无任何 viewer 时消耗 **约 0.006 核 + 4 MiB PSS + 7 FD + 2 线程**。
2. 该成本与阶段 2 冒烟测试测得的 0.071 核（含 1 FLV + 1 WHEP viewer）不同源：本项已确认 0 viewer，属于纯接入/解复用成本。
3. publisher 与 signaling 是独立进程，各自约 0.01 核与 0 核、26 MiB 与 14 MiB PSS，不计入 DUT 成本；后续容量结论只看 DUT。

## A3 publish + 1 FLV 校验 viewer

3 次，`perf_run` 完整流程（warmup 10 s + 窗口 30 s），fixture 与 A2 相同：

| 指标 | 中位数 | 相对 A2 增量 |
| --- | --- | --- |
| DUT CPU | 0.01368 核 | +0.0047 核 |
| DUT PSS | 19.5 MiB（最大 20.5） | 与 A2 同量级（噪声内） |
| FD | 34 | +1 |
| 线程 | 10 | 0 |
| 上下文切换增量 | 2 878 / 30 s | 与 A2 相同量级 |
| 媒体覆盖率 | 0.999996 | — |
| 首包延迟 p50 / p95 | 951 ms / 951 ms | — |
| 每 viewer 媒体速率 | 29.9 视频消息/s、46.9 音频消息/s | 与 fixture 一致 |

结论：单个 FLV viewer 的边际成本约 **0.005 核**（含一次 GOP 缓存起播突发），媒体真实且与 fixture 帧率一致。该值与阶段 3B 的 FLV 每 viewer 拟合斜率（2.3 mCPU）不同：A3 只有一次连接建立 + 一次 GOP 突发，稳态转发成本更低，因此阶段 3B 的斜率才是长期外推依据。

## 与阶段 2 的一致性

阶段 2 冒烟（`rtmp → flv:1 + whep:1`）测得 0.071 核 / 28 MiB PSS。按本次基线拆解：
0.0062（接入）+ 0.0023（FLV viewer 稳态）+ 0.063（WHEP 1 viewer）≈ 0.072 核，与阶段 2 实测 0.071 核一致，说明基线拆解可复现。
