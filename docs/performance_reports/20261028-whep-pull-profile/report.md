# WHEP Pull 输入性能与 Worker 分布专项审查

基线 `cb03380`，8 个物理核。本轮只做测量、源码审查和可复现实验，没有修改生产代码，没有提交或推送。

## 1. 结论

**判定：OPTIMIZE（Phase D 情形 A）。** 建议 WHEP Pull 会话按现有 WHEP Pull 会话数选择 worker。方案见第 12 节，本轮未实现，等待下一轮决定。

| 问题 | 答案 |
| --- | --- |
| 1. 8 个 worker 上的会话分布 | 会话跟随 `/pull/create` 所在的 HTTP 连接，而连接按全局 accept 计数轮转。只有建流请求时完全均匀（150 路为 19×6 + 18×2）；混入其他连接后近似随机，150 路实测为 25/22/22/18/18/17/14/14、26/21/20/20/20/18/14/11、25/23/22/20/19/18/12/11 |
| 2. HTTP 控制轮转是否影响 | 是。`workers.next()` 由 RTMP、RTSP、HTTP 三个监听共用，每个 FLV、HLS、WHEP、WHIP、RTMP、RTSP 连接和每个控制请求都会推进计数，`/pull/create` 落到哪个 worker 取决于此前的所有连接。控制接口对每个响应都设了 `keep_alive(false)`，不存在复用连接固定到同一 worker 的问题 |
| 3. 每 worker CPU 与会话数是否对应 | 线性对应，未饱和时每路约 0.105 Gcycles/s（13 路 1.39 G，19 路 1.99 G，14 路 1.47 G） |
| 4. 每路成本是否相同 | 同一内容下相同（25–160 路时为 105–112 Mcycles/s）；不同音频内容下差约一倍，avsync 素材每路约 50 Mcycles/s |
| 5. 成本构成 | AAC 编码 81.4%，Opus 解码 6.4%，内核 UDP 2.4%，SRTP、RTP 解复用和 H.264 合计 2.2%，内存拷贝与分配 2.3%，FLV 输出 0.4% |
| 6. 单 worker 是否提前饱和 | 是。mix125-s12 总周期只用到封顶能力的 72%，持有 22 路的 worker 已达 0.997 繁忙并丢包（约 10.7 万），观看滞后 2.1 s |
| 7. 最先限制容量的因素 | 单 worker 上的同步 AAC 编码。每个 worker 稳定上限约 20 路（繁忙约 0.95），21–22 路开始丢包 |
| 8. 是否需要按会话最少放置 | 需要。完全均匀时 8 核稳定 160 路；现有放置在混合流量下 125 路 1/3 通过，150 路 4/4 失败 |
| 9. 收益是否大于复杂度和生命周期风险 | 是。改动约 40 行，选择和计数在一把锁内完成，计数跟随会话对象的生命周期，不改变会话的单 worker 归属 |

测试工具本身有一个限制：perf_run 让上游 media_server 使用与服务端相同的 8 个线程。150 路时上游两个线程打满（1.000/0.994），这一组 150 路结果不能算作服务端容量。改为 16 线程上游后，最热线程不超过 0.80。

## 2. 基线与环境

| 项 | 值 |
| --- | --- |
| 仓库 | `main` = `origin/main` = `cb03380`，工作区干净 |
| 构建 | `cmake --build build -j8` 无新编译；`build/media_server` SHA256 `89f841b3f6c5…679294`，与测试机 `rt-wp/build/media_server` 相同（每轮 `pids.txt` 记录） |
| 测试机 | <TEST_HOST>，Xeon Silver 4216，16C/32T，CPU N 的 SMT 兄弟为 N+16 |
| 服务端 | CPU 0-7（兄弟 16-23 空闲），`--threads 8` |
| 客户端与上游 | CPU 8-15、24-31，netns 198.19.0.11 |
| 调频 | schedutil。低负载时同样的工作占用更多 CPU 时间（25 路 3.34 核，100 路 5.78 核），所以容量比较用周期数。满载时全核约 2.30 GHz，8 核封顶约 18.4 Gcycles/s |
| 接收缓冲 | 媒体输入 socket 256 KiB（`28b8ede`），未修改全局 sysctl |
| 测试包 | `rt-wp` = `rt-gbB` 的硬链接副本，只把 `bench/perf_run.py` 换成带两个测试选项的副本（第 3 节） |

## 3. 方法与工具

工具位于 `tools/`，测试机任务位于 `tools/test-host/`。

- **`perf_run_interleave.py`**：`bench/perf_run.py` 的副本，差异见 `perf_run_interleave.diff`。新增两个选项，不传时行为与原版相同：
  - `--pull-interleave-seed S`：每次 `/pull/create` 前，按种子随机发送 0–7 个 `GET /receivers`，模拟生产中混入的其他连接；
  - `--upstream-workers N`：上游 media_server 的线程数。
- **`wp.sh` / `wp.bt`**：在测量开始 10 s 后采集以下数据：
  - 8 s 逐线程 perf stat（cycles、instructions）；
  - 8 s bpftrace：每个 (tid, port) 的 `udp_recvmsg` 次数，即会话到 worker 的映射；`audio_transcoder::transcode` 和 `whep_pull_session::handle_srtp` 的耗时；上下文切换次数；
  - 前后三次 `ss -uanmp`（Recv-Q 与 drops），以及服务端、上游的逐线程 `/proc` 统计。
- **`wpsum.py`**：逐 worker 汇总会话数、包率、转码 CPU、SRTP 处理 CPU、丢包、Recv-Q、Gcycles/s 和上下文切换。
- **`wqsum.py`**：逐轮汇总状态、观看实时比和滞后、服务端周期与频率、最忙 CPU、上游最热线程、丢包、控制探测和音画偏差。
- **`leafcost.py`**：按 `perf script` 叶子符号分类，在 WHIP 版本基础上增加了 FLV 输出类别。
- **`san/scripts/whep_lifecycle.{sh,py}`**：本地生命周期测试，覆盖建立、删除、反复创建删除、异常输入、上游源结束和上游进程被杀。

每轮观看端为 HTTP-FLV，每个源一个观看者（`--outputs flv:N`），测量窗口 60 s，预热 15 s。

## 4. 创建路径与 worker 归属

1. `service.cpp:50`：accept 循环先调用 `workers.next()`，再在该 worker 上 accept。计数器 `worker_pool::next_` 由 RTMP、RTSP、HTTP 三个监听共用。
2. `http_session.cpp:240`：`/pull/create` 交给 `handle_pull_request(request, worker_, ...)`，`worker_` 就是当前 HTTP 连接所在的 worker。
3. `pull_http.cpp`，`handle_create`：`http:` URL 构造 `whep_pull_session(worker, config)`；`start_session` 检查 `stream_registry` 冲突，调用 `add_receiver_session` 后 `startup()`。
4. `whep_pull_session.cpp`：会话生命周期内的全部 I/O 都在这个 worker 上，包括：
   - resolver 和各类 timer；
   - `run` 协程（HTTP offer/answer、端口获取、`udp_transport`）；
   - `run_udp`；
   - `whip_media_receiver` 的同步 Opus 解码与 AAC 编码；
   - 关闭时 DELETE 上游资源。

结论：WHEP Pull 会话的 worker 等于建流 HTTP 连接的 worker。这与 `83bf251` 之前的 WHIP 相同。RTSP 和 RTMP Pull 走同一路径，但它们不转码，单路成本低，不在本轮问题范围内。

## 5. Worker 分布

数据来自 `raw/prof/*`，用 `wpsum.py` 汇总。会话数按 worker 从多到少排列。

| 运行 | 创建方式 | 每 worker 会话数 | 每 worker 繁忙 | 丢包/s | 合计 Gcycles/s | 结果 |
| --- | --- | --- | --- | --- | --- | --- |
| wq-seq150-r1 | 顺序 | 19×6, 18×2 | 0.85–0.90 | 0 | 15.71 | PASS |
| wq-seq160-r3 | 顺序 | 20×8 | 0.945–0.952 | 0 | 16.88 | PASS |
| wq-seq170-r3 | 顺序 | 22×2, 21×6 | 0.985–0.995 | 2468 | 17.87 | FAIL |
| wq-mix125-s11 | 混合 | 19,19,19,16,16,13,12,11 | 0.71–0.92 | 0 | 13.06 | PASS |
| wq-mix125-s12 | 混合 | 22,19,17,16,14,14,12,11 | 0.71–**0.997** | 1445 | 13.10 | FAIL |
| wq-mix125-s13b | 混合 | 约 22…7（由周期推算¹） | — | — | 13.22 | FAIL |
| wq-mix150-s11 | 混合 | 25,22,22,18,18,17,14,14 | 0.72–**0.998** | 5474 | 15.36 | FAIL |
| wq-mix150-s12 | 混合 | 26,21,20,20,20,18,14,11 | 0.71–**0.999** | 3556 | 15.18 | FAIL |
| wq-mix150-s13b | 混合 | 25,23,22,20,19,18,12,11 | 0.71–**0.997** | 7377 | 15.33 | FAIL |
| wp-mix150-s1² | 混合 | 24,22,22,20,17,16,15,14 | 0.74–**0.994** | 4911 | 15.46 | 上游干扰 |

¹ 该轮诊断脚本未取到服务端 pid，没有端口映射。逐线程周期为 2.30/2.19/1.89/1.78/1.57/1.57/1.05/0.74 G，按每路 0.105 G 换算约为 22 路到 7 路。
² 上游为 8 线程，只作参考。

顺序创建时服务端只收到 `/pull/create` 连接，轮转完全均匀。这是测试条件，不代表生产。

随机放置的模拟（`raw` 中无文件，计算见第 13 节）给出最重 worker 不超过 20 路的概率：100 路 0.91，125 路 0.37，150 路接近 0。实测 125 路 1/3 通过、150 路全部失败，与模拟一致。

## 6. 每 worker CPU 与会话数

- **未饱和时线性**：每路约 0.105 Gcycles/s，每个 worker 的周期数等于会话数 × 单路成本。例如 mix150-s11 中 14 路的 worker 为 2.95 G inst/s、0.72 繁忙，18 路为 3.83 G、0.85。
- **饱和时封顶**：worker 在约 2.29 Gcycles/s 处封顶，繁忙接近 1.0，上下文切换降到个位数每秒，说明线程已不再空闲等待。
- **进入饱和后的现象**：Recv-Q 从 0–50 KiB 升到 400–500 KiB，接收队列溢出丢包。`handle_srtp` 每包平均约 99–123 µs（含同步转码），单次最长 12–64 ms。
- **单 worker 稳定上限**：约 20 路。20 路时繁忙 0.95、0 丢包；21–22 路丢包。

## 7. 每路成本与成本拆解

### 7.1 单路成本

数据来自 `raw/runs/wq-cap*-seq-r1`，perf 素材（H.264 720p30 2 Mbps，AAC 48 kHz 双声道 128 kbps，上游转成 Opus）。

| 路数 | 25 | 50 | 75 | 100 | 125 | 150 | 160 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Gcycles/s | 2.80 | 5.38 | 8.14 | 10.99 | 13.34 | 15.82 | 16.80 |
| 每路 Mcycles/s | 112 | 108 | 109 | 110 | 107 | 105 | 105 |

- **与 WHIP 对比**：比 WHIP 报告中的约 70 Mcycles/s 高约 50%。原因是音频内容不同，不是 WHEP Pull 路径额外开销：同样用 avsync 素材时，150 路只有 7.44 G（约 50 Mcycles/s 每路）。
- **对放置的影响**：按会话数放置在不同内容混合时只是近似均衡，但比按连接轮转好得多。

### 7.2 成本拆解

`raw/prof/wq150-seq`：150 路，perf record 全部 8 个 CPU，499 Hz，15 s，按叶子符号分类。

| 类别 | 占繁忙周期 |
| --- | --- |
| AAC 编码（twoloop 量化 28.5%，band cost 共约 30%，cbrtf 6.6%，心理声学、TNS、M/S 等） | 81.4% |
| Opus 解码（pvq 等） | 6.4% |
| 未分类用户态 | 2.7% |
| 内核 UDP 收发 | 2.4% |
| 其他内核 | 2.0% |
| memcpy/memmove | 1.5% |
| H.264 与 RTP 解复用 | 1.5% |
| 分配释放 | 0.8% |
| SRTP 解密 | 0.7% |
| FLV 输出 | 0.4% |
| 重采样与 FIFO | 0.2% |

bpftrace 的独立测量：转码在每个 worker 上占 0.61 核（共 0.90 核），`handle_srtp` 合计（含转码）占 0.65 核。转码之外的 SRTP 解密、RTP 处理和接收入口约占 4%。

### 7.3 对照

| 对照 | Gcycles/s | 结论 |
| --- | --- | --- |
| 150 路 + 150 个 FLV 观看（wq-seq150-r1） | 15.71 | 基准 |
| 150 路，无观看（wq-in150-none） | 15.72 | FLV 扇出成本在噪声内 |
| 150 路，单声道 64 kbps 源（wq-mono150） | 15.93 | 没有下降：接收端写死 `audio_channel_count = 2`，单声道源仍按双声道 128 kbps 编码（见 13.1） |

本轮没有测纯视频源：perf_run 要求素材有两条轨，源就绪探测也要求双轨，需要改工具。纯视频的成本可以从拆解中推出：AAC、Opus 和重采样合计 88%，所以纯视频每路约为现在的 12%。

## 8. 容量

三种容量都是按 8 个物理核、perf 素材测得：

| 容量 | 顺序放置（完全均匀） | 现有放置 + 混合连接 |
| --- | --- | --- |
| 会话建立 | ≥200（seq200 的 200 路全部 `srtp started`） | 同左 |
| 持续正确处理（0 丢包） | 160 PASS；170 FAIL（2 次） | 125 通过 1/3；150 失败 4/4 |
| 稳定实时（实时比 ≥0.999、滞后 ≤250 ms、音画偏差恒定、控制 p99 ≤15 ms） | 160 | 约 100（随机放置下最重 worker ≤20 路的概率 0.91） |

顺序放置的扫描，带 `--control-probe-ms 200`：

| 路数 | 结果 | 实时比 | 滞后最大 | Gcycles/s | 最忙 CPU | 丢包 | 控制 p50/p99 ms | 首帧 p50/p95 ms |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 25 | PASS | 0.999 | 88 | 2.80 | 0.44 | 0 | 2.7/12.4 | 1155/1915 |
| 50 | PASS | 0.999 | 85 | 5.38 | 0.71 | 0 | 5.1/16.1 | — |
| 75 | PASS | 0.999 | 82 | 8.14 | 0.72 | 0 | 4.4/15.1 | — |
| 100 | PASS | 0.999 | 79 | 10.99 | 0.73 | 0 | 3.7/10.0 | 1074/1948 |
| 125 | PASS | 0.999 | 80 | 13.34 | 0.75 | 0 | 3.3/9.0 | — |
| 150 | PASS | 0.999 | 183 | 15.82 | 0.86 | 0 | 4.0/11.4 | 1023/1877 |
| 160 | PASS | 0.999 | 230 | 16.80 | 0.91 | 0 | 4.6/13.6 | 996/1870 |
| 170 | FAIL | 0.977 | 1748 | 17.81 | 0.99 | 99 099 | 5.4/39.3 | 1093/1959 |
| 180 | FAIL | 0.981 | 1736 | 18.34 | 1.00 | 595 119 | 8.6/84.2 | — |
| 190 | FAIL | 0.986 | 1721 | 18.43 | 1.00 | 1 287 558 | 35.4/120.6 | 1425/3928 |

- **复测**：seq160-r3 PASS、seq170-r3 FAIL，与扫描一致。
- **200 及以上**：seq200、seq250、mix200 在服务端源就绪阶段就失败。原因是 200 × 0.105 ≈ 21 G 已超过 18.4 G 的封顶，即使完全均匀也不够。
- **控制接口**：饱和后控制请求同样会被轮转到饱和的 worker 上，所以 p99 从约 14 ms 升到 39–121 ms。
- **首帧时间**：p50 约 1 s，p95 约 1.9 s，是在等关键帧（GOP 2 s），170 路以内不随负载变化。

## 9. 上游与测试工具限制

- **perf_run 的上游线程数**：perf_run 让上游 media_server 使用 `--threads args.workers`，即 8。上游每路要做 AAC→Opus egress 转码，150 路时两个上游线程到 1.000/0.994，其余约 0.5（`wp-seq150-r1` 被判 INCONCLUSIVE/CLIENT_RESOURCE）；200 路时上游源在第 186 路就绪失败。之前 a1a3 报告中“150 路 PASS，6.83 核”是在同样的 8 线程上游下测得的。
- **本轮的做法**：除标注外，本轮数据都用 `--upstream-workers 16`。上游最热线程在 150 路时 0.58–0.69，190 路时 0.68–0.80，没有成为瓶颈。
- **上游线程间不均**：16 线程上游的线程负载在 0.12 到 0.58 之间。这是上游 WHEP 播放侧 egress 的放置问题，不属于本轮范围，只作记录。

## 10. 下游扇出

- 每源 1 个 FLV 观看者的成本在噪声范围内（15.71 对 15.72 G），profile 中 FLV 输出占 0.4%。
- 输入成本几乎全部来自转码，所以 WHEP Pull 输入容量与下游扇出可以分开估算；扇出容量沿用各输出协议已有的报告结论。

## 11. 用户体验与正确性

**音画同步与时间线**（`raw/runs/wq-av*`）：avsync 素材，每轮 40 s 内 20 个标记点，四种输出均测。

| 场景 | 音画偏差 min/p50/max（FLV、RTMP） | RTSP | HLS | 视频 | 音频 |
| --- | --- | --- | --- | --- | --- |
| 1 路（2 轮） | −50.8/−50.8/−50.8 ms | −51.0/−50.7/−50.3 | −51.3 | 30.0 fps，pts 无回退、无缺口 | 无重叠 |
| 150 路顺序 | −34.8/−32.5/−32.5 ms | −35.0/−32.7/−32.0 | −33.0 | 29.98 fps，pts 最大间隔 54 ms，无回退 | 无重叠 |
| 150 路混合（seed 11） | −51.2/−48.8/−48.8 ms | −51.7/−49.3/−48.7 | −49.6 | 同上 | 无重叠 |

- **偏差恒定，不漂移**：每轮窗口内的波动不超过 2.5 ms，F1 行为保持（只用首个 SR 对齐，WebRTC 视频的 dts 等于 pts）。
- **偏差常数随会话不同**：在 −32 到 −51 ms 之间，取决于首个 SR 对齐时的时刻，在可接受范围内。
- **测试负载的局限**：avsync 素材每路成本只有 perf 素材的一半，150 路只用到封顶能力的约 40%（最忙 CPU 0.72），所以这组没有覆盖临界负载下的音画同步。临界负载下的实时性由第 8 节的 perf 素材扫描给出：160 路实时比 0.999、滞后 ≤245 ms；170 路以上已经丢包，音画同步失去意义。
- **解码与时间线**：
  - 四种输出的 ffprobe 解码全部正常（H.264 Constrained Baseline 720p，AAC LC 48 kHz 双声道）；
  - 所有 PASS 轮实时比 0.999，没有半速；
  - 滞后不随时间增长；
  - 只有 RTSP 输出在单路和 150 路时都出现过一次 5–33 ms 的音频 pts 缺口，与负载无关；
  - HLS 的 18 次到达停顿是分片拉取节奏，正常。

**生命周期**（本地，`san/runs/rel-r1/driver.txt`）：两个 release media_server 通过回环相连，服务端 4 线程，上游 2 线程。

| 场景 | 结果 |
| --- | --- |
| 建立 8 路 | create 全部 201，最长 2.8 ms；receiver ready 约 1.2 s；FLV 首个 tag 2.0 s |
| 删除 | 全部 204，最长 0.3 ms；receiver 立即消失；UDP socket 13 ms 回到基线；向上游的 DELETE 返回 204；重复删除返回 404 |
| 20 轮 × 8 路反复创建删除 | 160/160 创建 201、就绪、删除 204，socket 回到基线 |
| 重复 stream_id | 409 |
| 上游不可达或源不存在 | 201，随后自动清理：端口拒绝时立即清理；上游 409 时 20 ms 内清理 |
| 上游源结束（杀掉推流进程） | 100 ms 内清理，上游关闭会话 |
| 上游进程被 SIGKILL | 20.0 s 后由 idle 超时清理，socket 回到基线 |
| 服务端 SIGTERM | 退出码 0，3.5 ms |

测试机上所有轮次清理后 `remaining_owned_receivers` 均为空，进程全部回收。

## 12. 方案评估（Phase D）

### 12.1 判定

属于情形 A：分配不均导致单 worker 提前饱和。

- 现有放置使 125–150 路时最重 worker 达到 22–26 路，比每 worker 约 20 路的上限多 10%–30%。
- 总 CPU 只用到 72%–83% 时就开始丢包。
- 完全均匀时 8 核稳定 160 路。

在生产中，FLV、HLS、WebRTC 播放和控制请求都会推进同一个轮转计数，`/pull/create` 的落点无法预测。

### 12.2 建议方案：WHEP Pull 按会话数放置（本轮未实现）

**`media/webrtc/whep_pull_session.{h,cpp}`**
- 新增 `namespace whep_pull { void startup(worker_pool&); std::shared_ptr<whep_pull_session> create(whep_pull_config); }`。
- 文件内维护一个放置状态 `{std::mutex, worker_pool*, std::vector<std::size_t> sessions}`，只记每个 worker 上存活的 WHEP Pull 会话数。
- `create` 在锁内取会话数最少的 worker 并把计数加 1，然后用 `workers->context(index)` 构造会话。
- 会话保存 `worker_index_`，在析构时于锁内把计数减 1。
- 选择和加计数在同一把锁内完成，所以并发批量创建也不会扎堆到同一个 worker。

**`media/http/pull_http.cpp`**
- `http:` 分支改为 `start_session(request, *stream_id, whep_pull::create(...))`。
- RTSP 和 RTMP Pull 继续跟随 HTTP worker：不转码，单路成本低，没有证据表明需要调整。

**其他**
- `service.cpp`：在 `whip::startup(*workers_)` 旁边调用 `whep_pull::startup(*workers_)`。
- `docs/architecture.md`：补一句放置规则。

### 12.3 逐项审查

| 项 | 评估 |
| --- | --- |
| 与 RTSP/RTMP Pull 共用的接口 | `handle_create` 的签名不变，只改 `http:` 分支构造会话的方式 |
| registry 时序 | 不变：先查 `stream_registry` 冲突，再 `add_receiver_session`，最后 `startup()`。冲突时会话被 shutdown 并析构，计数随之归还 |
| 并发窗口 | 无：选择和加计数在一把锁内。如果改为按 receiver 注册数或按 `whip_media_receiver` 的存在来计数，要等协商完成（一次 HTTP 往返）后才会加计数，批量创建会全部选中同一个最少的 worker，因此不采用 |
| 统计语义 | 计数的是“存活的会话对象”，包括协商中和关闭中的会话。上游不可达时会多计最长约 15 s（建立超时），可接受 |
| socket、timer、resolver、协程的归属 | 不变：会话在构造时就拿到所选 worker，此后全部资源都在这个 worker 上，与现在一样不跨 worker |
| 与 WHIP 的关系 | 两套计数各自独立。只用其中一种时各自均匀；两者混用时，两组各自均匀相加仍然均匀。不复制 WHIP 的 `session_id → weak_ptr` 表，也不引入通用调度器或负载租约 |
| 规模 | 约 40 行生产代码，无新依赖、无新配置 |

### 12.4 预期收益与验证

**预期收益**：在混合连接的生产流量下，8 核 WHEP Pull 的稳定容量从约 100 路（125 路 1/3 通过、150 路 4/4 失败）提高到放置均匀时的 160 路，即 +28%～60%。如果只有建流请求、没有其他连接，现在已经是完全轮转，没有收益。

**下一轮验证**：
- **A/B**：用本轮工具对比 mix125/mix150 的 s11/s12/s13 与 seq160/seq170。预期 mix150 全部 PASS，顺序放置的边界保持 160/170。
- **生命周期**：重跑 `whep_lifecycle`，并在 ASan、TSan 下各跑一轮。
- **计数归还**：在建流冲突、上游失败和服务关闭三种路径下检查计数是否归零。

### 12.5 不采用的替代方案

- **按周期数放置**：需要逐 worker 的 CPU 测量和反馈，属于调度器，超出约束。
- **把转码移出 worker**：会引入线程池，被约束排除。
- **只记录边界不修改（情形 B）**：现有放置会在总 CPU 只用到 70% 左右时就失败，成本收益比远不如 12.2。

## 13. 其他发现与候选项（均未实施）

1. **单声道源按双声道编码**（`whep_pull_session.cpp:324`）：
   - 现状：接收端写死 `audio_channel_count = 2`，`parse_whep_pull_answer` 也不读 `sprop-stereo`。单声道源（`sprop-stereo=0`）仍按双声道 128 kbps AAC 编码，实测成本与立体声相同（15.93 对 15.71 G）。
   - 方案：改为按 answer 的声道提示配置。
   - 预期：单声道源的 AAC 编码大约减半，且不降低质量，因为源本身就是单声道。
   - 前提：需要单独审查 `sprop-stereo` 的语义（它只是发送端的提示），以及发送端实际发立体声时的行为。
2. **AAC 编码器参数**（即 891aec1 报告中的 A4）：AAC 编码占 81%。改用 fast 量化器可以大幅降低成本，但会改变音质，需要单独做音质评估。本轮不用降低质量来换取收益。
3. **测试工具**：
   - 建议把 `--upstream-workers` 加入 `bench/perf_run.py`。否则 WHEP Pull 容量测试会先在 8 线程上游上饱和，测到的是上游的上限。
   - `--pull-interleave-seed` 用于复现生产中的混合放置，也建议保留。
   - 这两项只改测试工具，可以单独提交。
4. **上游 WHEP egress 线程不均**：见第 9 节，属于 WHEP 播放侧，不在本轮范围。

随机放置的模拟：每路独立均匀地落在 8 个 worker 之一，每组 2 万次。

| 路数 | 最重 worker 路数 p50 | p90 | P(≤20) | P(≤21) | P(≤22) |
| --- | --- | --- | --- | --- | --- |
| 100 | 17 | 20 | 0.91 | 0.95 | 0.98 |
| 125 | 21 | 24 | 0.37 | 0.56 | 0.72 |
| 140 | 23 | 27 | 0.05 | 0.16 | 0.34 |
| 150 | 25 | 28 | 0.00 | 0.02 | 0.11 |
| 160 | 26 | 30 | 0.00 | 0.00 | 0.01 |

## 14. 数据索引与复现

| 数据 | 位置 |
| --- | --- |
| 每 worker 诊断（会话映射、逐线程周期、bpftrace、ss） | `raw/prof/wp-*`（8 线程上游）、`raw/prof/wq-*`（16 线程上游） |
| 对应的 perf_run 结果 | `raw/runs/d-wp-*`、`raw/runs/d-wq-*` |
| profile | `raw/prof/wq150-seq`（`perf-script.txt.gz`、`report-sym.txt`、`report-children.txt`、`perf-stat.txt`） |
| 容量扫描 | `raw/runs/wq-cap{25..190}-seq-r1` |
| 音画同步与解码 | `raw/runs/wq-av1-r1b`、`wq-av1-r2b`、`wq-av150-seq-r1b`、`wq-av150-mix11-r1b` |
| 生命周期 | `san/runs/rel-r1/`；单声道素材 `san/fixtures/perf_fixture_720p30_mono.flv`（SHA256 `6ca78c90…f1da41a`） |

- **测试机任务**：`tools/test-host/400…410`，按顺序执行。
  - 401、402 用的是 8 线程上游，已取消，没有运行；
  - 408 因磁盘预算被拦下，改为 410 重跑；
  - 407 的后 4 轮同样被拦下，改为 409 重跑。
- **磁盘处理**：本次任务的 `result.json` 在测试机上已就地 gzip 压缩，本地已同步一份。
- **同步**：`tools/sync.sh`。它会排除 credentials、token 和推流、就绪探测日志，本地 `raw/` 中经检查不含上述文件。

复现示例（测试机，`T=/var/tmp/ms-perf-891aec1`）：

```
RT=rt-wp $T/wp.sh <name> --input whep_pull --sources 150 --outputs flv:150 --pull-interleave-seed 11 \
    --upstream-workers 16 --perf-stat --warmup 15 --duration 60 --ready-timeout 300 --ramp-per-second 200 --group-size 256
python3 tools/wpsum.py raw/prof/<name>
```
