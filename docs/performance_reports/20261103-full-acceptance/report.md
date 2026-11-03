# 全协议正确性验收与固定 8 核混合负载（2026-11-03，`0eae004`）

结论：**GO**。WHEP 协商与生命周期边界、完整协议矩阵与 Sanitizer 子集均通过；HEAD 与历史 `3b6c0bd` 在 A/B/C 混合下性能一致，新增纯视频观众混合 D 的健康容量由改动前的 1.8× 提高到 2.2×（+22%），同档 cycles 少约 10%。Profiling 没有找到能归因、超出噪声的第一方热点，本轮未修改生产代码。

标记口径：PASS / FAIL / XFAIL / XPASS / BLOCKED / INCONCLUSIVE。未注明的结果均为本轮实测；“历史”指既有报告；“推断”单独注明。

## 1. Git、构建与代码变更

| 项 | 值 |
| --- | --- |
| 被测 HEAD | `0eae004`（`63191b5` 纯视频 WHEP 伴随流 + 报告提交） |
| `build/media_server`（HEAD） | sha256 `2b1efdc7f73c3f94…` |
| `build/media_server-a`（`79f9b23`，纯视频改动前） | `f24e6ca2bf0a4fd9…` |
| `build/media_server-hist`（`3b6c0bd`，10-30 混合负载报告） | `b95752baab1bc67d…` |
| ASan / TSan | `0b43db4a7794124a…` / `2812ff91102523fe…` |
| `build/whep_fanout` | `1836a5d5cc0f10c7…` |
| 上游 media_server（`rt-e1`） | `9d37de83e02af936…`，与历史相同 |
| 构建 | RelWithDebInfo，GCC 16，`deps/install`（OpenSSL 3.5.9、Boost 1.92、jemalloc 5.3.0），静态运行时；sanitizer 用 `build-san-address`、`build-san-thread` |
| 环境 | `<TEST_HOST>`，内核 6.8.0-84，schedutil；DUT 绑 CPU 0-7、8 worker，客户端 8-15,24-31；netns 内 DUT 198.19.0.10 / client 198.19.0.11；sysctl 未改 |
| 素材 | `perf_fixture_720p30` sha256 `59c81471…`、`perf_fixture_whip` `8581fcd9…`，与历史一致 |

生产代码（`media/`、`signaling/`、`web/`）未修改。未提交的工具改动：

- `bench/whep_fanout.cpp`：新增 `--unsupported-audio`（offer 的音频只含 G.722）；“是否期待音频”统一为 `expects_audio()`。
- `bench/perf_run.py`：输出类型新增 `whep_video`（`whep_fanout --video-only`），播放路径与 `whep` 相同。
- `regression/protocol_matrix.py`：新增 `whip-video-only-whep`、`whep-pull-video-only-whep`，共 35 个场景。
- `docs/performance.md`：说明 `whep_video` 与 `--unsupported-audio`。
- 本目录：`whep_boundary.py`（Chrome/原生 WHEP 协商边界）、`life_summary.py`、`mix_table.py`、`jobs/*.sh`（全部运行命令）。`mix.sh` 引用的 `sum.py` 与 profiling 分类用的 `classify_profile.py` 复用 [20261030-mixed-workload](../20261030-mixed-workload/report.md) 的同名工具。

## 2. 协商、浏览器、协议与生命周期矩阵

### 2.1 WHEP 协商边界（`whep_boundary.py`，`jobs/g01-boundary.sh`）

DUT 4 worker、debug 日志；另起一个 media_server 作上游。两个源：WHIP 直推（whip）与 DUT `/pull/create` 主动 WHEP Pull 上游（pull）。每个源接入真实 Chrome 三种 offer（仅视频 recvonly、只给 PCMU、音视频）和 `whep_fanout` 三种 offer（`--video-only`、`--unsupported-audio`、默认）；1/3 时刻加入 FLV 观众，2/3 时刻 DELETE whip/video 的 Chrome 观众，结束时 SIGTERM 两个发布端。

| 检查 | 结果 | 证据（Release 第 1 次，其余 4 次相同） |
| --- | --- | --- |
| Chrome 仅视频 offer | PASS | answer 只有 `m=video`；1280x720 30 fps，freeze 0、丢帧 0、丢包 0，音频包 0 |
| Chrome PCMU offer | PASS | answer `m=audio 0 … 0`；视频同上，音频包 0 |
| Chrome 音视频 offer | PASS | Opus 111；约 2864 个音频包 |
| 原生 G.722 / 仅视频 offer | PASS | 音频 0、视频 22315 包；默认 offer 音频 2800 包 |
| 主动 Pull 上游下的同组观众 | PASS | 与 whip 源相同 |
| Opus→AAC 启动（按阶段累计） | PASS | [2, 2, 4, 4, 4]：源就绪各 1，FLV 加入各 +1，WHEP 观众 0；AAC→Opus 0 |
| Chrome DELETE | PASS | 204，1 s 内服务端会话关闭 |
| 源结束与回收 | PASS | WHEP 会话 started 12 / shutdown 12（随源结束 5）；receivers 0.5 s 内清空；fd 20→34→20 |
| 重复 | PASS | Release ×3、ASan、TSan；退出码 0，无 sanitizer 报告（ASan 只有 swapcontext 警告） |

### 2.2 生命周期 churn（`jobs/g02-lifecycle.sh`）

`perf_run`：`whip:8,whep_pull:8` → `whep_video:32,whep:16,flv:4`，`--churn whep_video:16`（5 轮）、`--source-churn` whip（SIGKILL 与 SIGTERM 交替），debug 日志。

| 构建 | 结果 | realtime | 控制 p99 | WHEP started/shutdown | 结束 fd（基线后） | 残留 receiver |
| --- | --- | --- | --- | --- | --- | --- |
| Release ×2 | PASS | ≥0.9996 | 2.4 ms | 152/152 | 32 | 0 |
| ASan | PASS | 1.0 | 9.6 ms | 92/92 | 33 | 0 |
| TSan | PASS | 0.9995 | 29.6 ms | 92/92 | 32 | 0 |

测量窗口内 80 个纯视频 churn 会话触发 Opus→AAC 0 次；窗口内的启动全部来自源 churn（每轮新源 2 次）。sanitizer 无报告。

观察（未修复，非缺陷）：每个 WHIP 会话 `webrtc whip session shutdown` 日志打两次（DELETE 与接收循环结束都投递 `safe_shutdown()`）。第二次只重复重置已为空的对象，端口释放受 `local_port_` 守卫，注册表按对象身份移除；FD、端口与 receiver 计数均正常回收。消除它需要新增状态，不在本轮范围。

### 2.3 完整协议矩阵（`jobs/g03-matrix.sh`）

| 构建 | 场景 | 结果 |
| --- | --- | --- |
| Release | 35（含两个新纯视频场景） | 32 PASS，3 XFAIL：`rtsp-tcp-h264-aac`、`rtsp-udp-h264-aac`、`rtsp-tcp-h265-aac` |
| ASan | 22 | 20 PASS，2 XFAIL（RTSP AAC）；30 份 sanitizer 日志只有警告 |
| TSan | 12 | 11 PASS，1 XFAIL（`rtsp-udp-h264-aac`）；0 报告 |
| CTest | 0 | 未提供可运行测试（不计 PASS） |
| `signaling` | — | `go test` 无测试文件；`go vet`、`go build` 通过 |

RTSP AAC 的严格 XFAIL/XPASS 规则与阈值未改动。

## 3. 8 核混合负载健康容量

运行命令见 `jobs/mix.sh`（参数 `COMMON` 与 10-30 报告一致：8 worker、60 s 测量、15 s warmup、`--source-churn 20`、`--churn rtmp:100`、`--av-probe 20`、`--perf-stat`、`--control-probe-ms 200`）。混合 D 为本轮新增：

```
MIX_D_IN=whip:48,whep_pull:48,rtmp:32
MIX_D_OUT=whep_video:640,whep:256,flv:32,rtmp:32,hls:8
```

判定列 `PASS(gb-probe)`：`perf_run` 判 FAIL 的唯一原因是 GB 音画探针要求同步标记且不容许音频缺口，而负载素材没有标记、循环接缝处有一处约 33 ms 缺口（FLV/RTMP/HLS 探针同样观察到该缺口）。其余检查（UDP、就绪、realtime≥0.98、churn、解码、清理）全部通过时才这样标记。这是 harness 与负载素材不兼容，历史二进制表现相同，不归因于服务端。

| 混合 | 健康档（HEAD） | 失败档（HEAD） | 历史 10-30 边界 | 判定 |
| --- | --- | --- | --- | --- |
| A | 1.0×（3/3，含历史二进制） | 1.05×：2/2 UDP 丢包 13–23 | 1.05× | 当前 harness 下 1.0×；历史二进制在 1.05× 也 1/2 丢包，两版本一致 |
| B | 1.6×（4/4，busiest 0.92） | 1.7×：2/2 UDP 丢包 | 1.6× | PASS，与历史一致 |
| C | 2.15×（服务端健康，PASS(gb-probe)） | 2.3×：饱和，丢包 146161 | 2.15× | PASS，与历史一致 |
| D | 2.2×（2/2，busiest 0.91） | 2.4×：2/2 丢包 | 新增 | 改动前 1.8×，见 3.1 |

PSS、FD、控制 p99（健康档）：A 1.0× 1.46 GiB / 6544 / 12–17 ms；B 1.6× 0.78 GiB / 2901 / 6.5–8.2 ms；C 2.15× 1.31 GiB / 3203 / 8.5–24.3 ms；D 2.2× 1.12 GiB / 2550 / 10.0 ms。所有运行（含失败档）清理后 fd 回到 32，无残留 receiver，churn 失败 0。

### 3.1 纯视频混合 D：HEAD 与改动前 `79f9b23`

| 档位 | 改动前 | HEAD |
| --- | --- | --- |
| 1.0× | PASS，10.29 / 10.27 Gcyc/s，14.2 Ginstr/s | PASS，9.14 / 9.41 Gcyc/s，12.6–12.7 Ginstr/s |
| 1.5× | — | PASS，13.16 Gcyc/s，busiest 0.74 |
| 1.6× | PASS，15.84 Gcyc/s，busiest 0.83 | — |
| 1.8× | PASS ×2，17.43 / 17.59 Gcyc/s，busiest 0.92 | — |
| 2.0× | FAIL ×2（busiest 1.00，丢包 22443 / 39213） | PASS ×2，16.6 Gcyc/s，busiest 0.85 |
| 2.2× | FAIL（丢包 612132） | PASS ×2，17.72 / 17.81 Gcyc/s，busiest 0.91 |
| 2.3× | — | PASS，busiest 0.95（高于其余健康档，余量小） |
| 2.4× | — | FAIL ×2（丢包 4875 / 35996） |

1.0× 时 cycles 少 10%、instructions 少 11%（schedutil 下 cycles 受频率影响，instructions 更稳定）；在 busiest≈0.91–0.92 的可比健康点，改动前为 1.8×、HEAD 为 2.2×，健康容量提高 22%；HEAD 2.3× 仍通过但余量小，2.4× 稳定失败。

## 4. 与既有基线的可比结果

- 复现（`jobs/g04-repro.sh`，`rt-p` 原工具 + `3b6c0bd`）：B 1.6× 17.68 Gcyc/s、busiest 0.921；A 1.05× 17.37、0.853；C 2.15× 17.38、0.945；全部 PASS，与 10-30 报告一致（±1%），基线可比，**不是 INCONCLUSIVE**。
- A/B（`jobs/g05-ab.sh`，新工具，ABBA）：

| 混合 | 历史 `3b6c0bd` Gcyc/s | HEAD Gcyc/s | 判定 |
| --- | --- | --- | --- |
| B 1.6× | 17.63、17.70 | 17.59、17.67 | 均 PASS，差 <0.5% |
| A 1.05× | 17.35、17.34 | 17.30、17.33 | 两版本都在丢包边缘（hist 0/42，head 23/13） |
| C 2.15× | 17.43、17.37 | 17.38、17.43 | 均 PASS(gb-probe) |

A/B/C 中每个源都有 AAC 观众，纯视频改动不减少转码，结果符合预期：无收益、无退化。A 在新 harness 下 1.05× 两版本都偶发丢包，而复现用的旧 harness 通过；新 harness 增加了 `--av-probe` 等客户端，推断为测量负载差异，不是服务端回归。

## 5. Profiling（`jobs/g09-prof.sh`）

测量开始 25 s 后 `perf record -F 499 -g -C 0-7` 15 s，再 `perf stat` 10 s。分类（占服务端 CPU 样本 %）：

| 类别 | D 2.0 HEAD（健康） | D 2.0 改动前（饱和） | D 2.4 HEAD（饱和） | B 1.6 HEAD | A 1.0 HEAD |
| --- | --- | --- | --- | --- | --- |
| kernel（UDP 收发、softirq、epoll） | 41.6 | 33.8 | 40.4 | 37.3 | 56.3 |
| opus 编码（AAC→Opus） | 13.0 | 12.3 | 13.5 | 10.7 | 10.6 |
| SRTP/加密 | 9.8 | 8.7 | 10.4 | 6.9 | 2.4 |
| media_server 其他 | 8.3 | 8.3 | 8.4 | 7.6 | 5.5 |
| AAC 编码（Opus→AAC） | 8.2 | 16.0 | 7.4 | 14.2 | 0.9 |
| H264/RTP/封装 | 5.5 | 5.5 | 6.0 | 6.4 | 9.0 |
| opus 解码 | 2.9 | 4.9 | 2.5 | 4.3 | 0.2 |
| asio / alloc / mutex / atomic | 5.4 | 4.9 | 6.0 | 7.5 | 9.4 |

| 运行 | cycles（10 s） | instructions | context-switches |
| --- | --- | --- | --- |
| D 2.0 HEAD | 172.9 G | 235.0 G | 285306 |
| D 2.0 改动前 | 189.5 G | 275.9 G | 1676（全核饱和、事件循环不再睡眠） |
| B 1.6 HEAD | 178.0 G | 226.2 G | 206712 |
| A 1.0 HEAD | 165.1 G | 161.2 G | 279450 |

- D 下 HEAD 与改动前的差别集中在 AAC 编码（16.0%→8.2%）和 Opus 解码（4.9%→2.9%），即纯视频观众不再触发 Opus→AAC；其余类别占比基本不变。
- 第一方符号无单项 >0.7%：`webrtc_packetizer::on_packet` 0.66%、`srtp_transport::protect_rtp` 0.61%、`udp_transport::write` 0.47%、`worker_sink_dispatcher` 三项合计约 1.1%。
- 调度：各 worker 负载 max/min 在健康档相差 ≤0.04 核（如 D 2.0 0.85/0.84），无单 worker 热点；饱和档全部 worker 同时到 1.0。
- 队列与网络：健康档 `queue_full`、`no_buffer_space`、UDP drops 均为 0；失败档的丢失全部是 DUT 接收缓冲溢出（`UdpRcvbufErrors`），发生在 CPU 饱和之后。
- 内存：健康档 PSS 0.78–1.46 GiB，清理后回落（D 2.0 运行 331→289 MiB）。

## 6. 优化：实施与否决

| 候选 | 证据 | 决定 |
| --- | --- | --- |
| SRTP 重放窗口 1024→更小（`srtp_transport.cpp` `window_size`） | libsrtp `bitvector_left_shift` 0.94%（D 2.0） | 否决：上限约 0.9% cycles，低于同配置重复运行的 1–3% 差异，收益不可归因；且会缩小 WHIP 接收的乱序容忍度 |
| UDP 批量发送（sendmmsg/GSO） | kernel 37–56%，`udp_sendmsg`、`__sys_sendto`、`__ip_select_ident` 等每包路径分散 | 不在本轮实施：需要改变 transport 写路径与 buffer 生命周期，不是最小修改；列为后续候选 |
| 转码器共享与编码参数 | opus 编码 11–13%、AAC 编码 7–16% | 已是每源最多一个方向一个转码器；空闲转码器释放方案已被否决；AAC afterburner 已关闭 |

结论：本轮没有满足“问题复现→可归因收益”条件的生产优化，停止优化。

## 7. 未解决问题与剩余风险

- GB 音画探针与负载素材不兼容（缺同步标记、接缝缺口），混合负载下 `perf_run` 会把 GB 观众判 FAIL。应为负载素材生成同步标记或在混合负载中对 GB 用不要求标记的探针；本轮只在报表中区分，未改 harness 判定。
- A 混合在当前 harness 下的边界为 1.0×，比旧 harness 低一档；两版本一致，属于测量负载差异（推断）。
- WHIP 会话 shutdown 重复执行一次（见 2.2），目前无资源后果。
- 容量边界只覆盖本 8 核测试机与 loopback/netns 网络；真实网卡、跨机丢包与 GSO 未测。
- 每个档位多数只有 1–2 次重复，边界附近（D 2.3×、A 1.0×）结论置信度低于中间档。

## 8. GO / NO-GO

**GO**。阶段 1、2 全部 PASS（RTSP AAC 为已知 XFAIL）；A/B/C 与历史基线一致；纯视频观众混合的健康容量由 1.8× 提高到 2.2×；没有本轮回归、资源泄漏或 sanitizer 报告。
