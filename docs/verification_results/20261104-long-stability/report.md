# 长期稳定性与真实设备兼容性验收（2026-11-04）

**结论：服务端长期稳定性 GO，NO CHANGE；真实 GB28181 摄像机兼容性 BLOCKED，因此整体 INCONCLUSIVE。** 本轮没有发现生产缺陷，不修改代码。

标注：**[实测]** 为本轮在测试机上的测量；**[历史]** 为既有报告中的证据；**[推断]** 为根据代码或数据作出的推理，未经直接测量。

## 1. 版本与环境

| 项 | 值 |
| --- | --- |
| HEAD | `830668b`（与 origin/main 一致）。本地工作区另有未提交的 RTSP AAC 调查报告、检查程序和 README 索引，不影响生产代码 |
| 被测 ELF | `build/media_server`，sha256 `ea058ec94b5917e3…`。版本串为 `93bb9eb…-dirty`：构建时 `830668b` 尚未提交，`93bb9eb..830668b` 在 `docs/` 之外的差异正好是构建时工作区里的 4 个文件，生产源码与 HEAD 一致。2026-11-05 补充核对：4 个生产文件的 mtime（最晚 13:27:24）都早于这个 ELF 的生成时间（13:27:30），内容与 HEAD 相同；在同一个 `build-rel` 中按当前 HEAD 增量重建，只重新编译了 `build_info.cpp`，得到 `e86cd9c3505f9f5b…`（版本串 `830668ba4b8f-dirty`）。逐字节比较两个 ELF，只有 30 字节不同：20 字节是 GNU build-id（偏移 945–964），10 字节是版本串中提交哈希的字符（偏移 15702728 起），其余完全相同 |
| 依赖（启动日志） | gcc-16 16.0.1 20260315，Boost 1.92，OpenSSL 3.5.9，FFmpeg n9.0.1，libopus 1.6.1，libsrtp2 2.7.0，jemalloc 5.3.0 |
| 测试机 | `<TEST_HOST>`，Xeon Silver 4216，32 线程，93 GiB 内存，内核 6.8，CPU 调频策略 schedutil |
| 拓扑 | 已有的 netns 198.19.0.10/11；服务端 8 个 worker，绑定 CPU 0–7；客户端绑定 8–15 和 24–31。没有调整 sysctl、内核队列或网络参数 |
| 运行包与素材 | 测试机上的 `rt-r` 运行包；负载素材 `perf_fixture_720p30.flv` 和 `perf_fixture_whip.mkv`（120.054 s，音频 6002 包连续） |

## 2. 复用与未重复的内容

- **直接复用**：
  - `bench/perf_run.py`；
  - 混合负载 D/B 的配置（测试机 `jobs-lib/mix.sh` 的 `mixrun`，含 `--churn rtmp:100`、`--source-churn 20`、`--av-probe 20`、`--perf-stat`）；
  - 生命周期 job 的配置（与 `20261104-port-pool-replay/jobs/r03-verify.sh` 的 life-whep/life-gb 相同）；
  - `life_summary.py`、`sum.py`、`dumpseq.py`。
- **新增**：
  - [`series.py`](series.py)：只读 `result.json`，按 10 个时间桶输出 PSS、RSS、FD、CPU、发送队列和 UDP drop，并计算后半窗口的斜率以及首尾四分之一的中位数。
  - [`jobs/`](jobs/)：本轮的队列任务。
- **未重复执行**：
  - 19 项库优先审查；
  - 35 场景协议矩阵；
  - Sanitizer、Chrome WHEP 边界测试和容量搜索。

  生产代码自上一轮完整验证（[20261104-port-pool-replay](../20261104-port-pool-replay/report.md)：32 PASS + 3 XFAIL，ASan/TSan 无报告）以来没有变化 [历史]。

## 3. 长稳场景 [实测]

| 场景 | 持续 | 负载 | 结果 |
| --- | --- | --- | --- |
| A 混合负载 D 1.6× | 1800 s | 输入 whip 77、whep_pull 77、rtmp 51；输出 whep_video 1024、whep 410、flv 51、rtmp 51、hls 13；rtmp churn 100 人/20 s；源 churn 20 s | PASS HEALTHY |
| B 混合负载 B 1.2× | 1800 s | 输入 whip 58、whep_pull 58、rtmp 38；输出 whep 768、flv 768、rtmp 384；churn 同上 | PASS HEALTHY |
| C1 WHEP churn | 600 s | whip 8、whep_pull 8 → whep_video 32、whep 16、flv 4；whep_video churn 16 人/20 s；WHIP 源 churn | PASS HEALTHY |
| C2 GB churn | 600 s | rtmp 8 → gb_udp 256、whep 16、flv 4；gb_udp churn 64 人/20 s；RTMP 源 churn | PASS HEALTHY |
| C3 RTSP churn | 600 s | rtmp 4、rtsp_tcp 4、rtsp_udp 4 → rtsp 64、whep 8、flv 4；rtsp churn 32 人/20 s；RTMP 源 churn | PASS HEALTHY |
| C4 RTMP churn | 600 s | rtmp 8 → rtmp 64、flv 32、whep 8；rtmp churn 64 人/20 s；RTMP 源 churn | PASS HEALTHY |

档位选择：

- 历史上的健康容量是 D 2.2×、B 1.6× [历史]；本轮 D 取 1.6×、B 取 1.2×，以保留 CPU 余量。
- 服务端 worker 的忙碌率约为 0.75（D）和 0.74（B），CPU 没有饱和。
- 客户端单个 CPU 的最高忙碌率为 0.41/0.39，没有出现 CLIENT_RESOURCE 或 BLOCKED。

### 3.1 观看者推进

| 场景 | ready / progressing | runtime 失败 | SRTP unprotect 失败 | 实时推进比例最小值 | lag span 最大 |
| --- | --- | --- | --- | --- | --- |
| D | 1549/1549 | 0 | 0 | 0.999987 | 79 ms |
| B | 1920/1920 | 0 | 0 | 0.999987 | 80 ms |
| C1 WHEP | 52/52 | 0 | 0 | 0.999951 | 78 ms |
| C2 GB | 276/276 | 0 | 0 | 1.00001 | 132 ms（gb_udp） |
| C3 RTSP | 76/76 | 0 | 0 | 0.99995 | 82 ms |
| C4 RTMP | 104/104 | 0 | 0 | 0.99995 | 79 ms |

- churn 轮次全部成功：D 87/87，B 87/87，C1–C4 各 29/29。
- 源 churn 全部成功：D 89/89，B 89/89，C1 23/23，C2–C4 各 29/29。
- 观看者在源消失后最多 1.2 ms 内结束，没有残留。

### 3.2 资源时间序列（10 个时间桶的中位数，每秒 1 个样本）

| 场景 | PSS 首/尾四分之一 MiB | 后半斜率 MiB/h | FD 中位数（全程） | CPU 核（各桶） | worker CPU 核 |
| --- | --- | --- | --- | --- | --- |
| D | 812.7 / 813.6 | −7.9 | 1886（各桶不变） | 5.96–5.98 | 0.739–0.756 |
| B | 597.7 / 598.2 | −3.4 | 2206（各桶不变） | 5.84 | 0.725–0.739 |
| C1 | 60.1 / 60.9 | +2.4 | 118 | 0.82 | 0.078–0.125 |
| C2 | 49.3 / 50.1 | −24.4 | 700 | 1.44 | 0.176–0.187 |
| C3 | 61.3 / 62.7 | −4.4 | 168 | 0.80 | 0.094–0.109 |
| C4 | 69.8 / 68.2 | −35.7 | 208 | 0.89 | 0.104–0.124 |

- 每个桶内 PSS 的波动在 ±2 MiB 以内；FD 中位数从头到尾不变，FD 最大值的跳动来自 churn 期间的连接，在每轮结束后回落。不存在单调增长。
- 各 worker 的 CPU 偏差在 3% 以内（D/B），分布均匀。
- 发送队列高水位（服务端 socket）：D 的 TCP 最大 132 KiB、UDP 最大 4 KiB；B 的 TCP 最大 216 KiB；C4 的 TCP 单次最大 588 KiB，之后回落。全程 `queue_full` 0、`no_buffer_space` 0、UDP drops 0、UdpRcvbufErrors 0。
- 控制面延迟：D 共 8985 次请求，p50 1.9 ms、p99 5.3 ms、p100 503 ms，其中 > 50 ms 的 4 次（134/169/405/503 ms）；B p100 431 ms，> 50 ms 的 2 次。两者均无错误，属于孤立尖峰，不是持续退化。2026-11-05 归因（只用现有样本）：`control_probe` 是 runner（`perf_run.py`）进程内的线程，测的是端到端 HTTP 耗时。D 中最大的 3 次尖峰（134/405/503 ms）与 runner 同一时刻采样轮次里的单步停顿（134/406/504 ms）在 1 ms 内吻合，这 3 次停顿都发生在读取客户端 publisher 进程的 `/proc` 快照时，而不是在读取服务端时。同期服务端每秒 CPU 连续（5.8–6.5 核），CPU PSI 为 0，前后请求为 1–5 ms，观看者的实时推进不受影响。尖峰与每 5 秒一次的 socket 表采样不重叠。结论：这是测量进程 runner 自身的停顿，不是服务端处理延迟；内核层面的停顿原因（需要 off-CPU 追踪）没有继续追查。另外 3 次（D 169 ms，B 366/431 ms）没有对应的采样停顿，原因不明；它们同样没有超时、失败或积压。

### 3.3 清理后回落

| 场景 | 基线 FD → 清理后 | 基线 PSS → 清理后 MiB | 残留 receiver | 线程 |
| --- | --- | --- | --- | --- |
| D | 237 → 32 | 263.0 → 290.5 | 0 | 16 |
| B | 186 → 32 | 204.1 → 190.6 | 0 | 16 |
| C1 | 48 → 32 | 41.3 → 42.1 | 0 | 16 |
| C2 | 40 → 32 | 20.5 → 43.2 | 0 | 16 |
| C3 | 60 → 32 | 29.6 → 57.1 | 0 | 16 |
| C4 | 40 → 32 | 20.3 → 57.8 | 0 | 16 |

- FD 全部回到 32（监听套接字、日志等常驻 FD），没有 socket 泄漏。
- 清理后的 PSS 比启动基线高出 0.8–37.5 MiB，但不随运行时长增长：600 s 与 30 s 冒烟测试的清理后 PSS 相当（RTMP：57.8 对 60.6 MiB），1800 s 的 B 甚至低于基线。[推断] 这是 jemalloc 保留的空闲页与各 worker 内存池；时间序列上没有泄漏的形态。

### 3.4 日志与异常

- `runs11` 中 8 次运行（6 个场景与 2 次冒烟测试）的服务端日志共有 47943 行 info、40634 行 debug，**没有 warning、error 或 critical**。
- SRTP 相关日志只有启动与握手记录，没有 `srtp protect failed`。
- 没有异常关闭、超时或媒体停止。

### 3.5 音画探针

- FLV/RTMP/HLS 的 av_probe（负载素材，非 avsync 素材）：视频和音频时间戳都没有缺口，没有解码警告。
- WHEP 探针在 D、B、C1 中各报告 1 次 `audio_pts_gaps`，最大 80 ms。专项复现结论：**这是探针录制工具的伪影，不是服务端缺陷**。
  - `s3-whep-gap-r1..r3`：同一配置跑 150 s，3 轮中 2 轮报告 80 ms。
  - 用 `dumpseq.py` 直接检查服务端发往观看者的 RTP 录制（whep-0.dump）：序号全部连续；Opus 时间戳步长除 92 s 处的一组（1968、1008、912）外全部为 960；探针只回放前 25 s，在这一窗口内完全规则。
  - 92 s 处那组步长合计多出 1008 样本（21 ms），三轮出现在同一位置，与 WHIP 测试推流端循环 120.054 s 素材时的尾部空隙一致 [推断]。它属于输入侧，并且不在探针窗口内。
  - 在负载之外，把同两份 dump 再回放 6 次：都没有缺口。
  - 运行期间的录制文件 `whep-0.mkv`：两份报告 80 ms 的录制里，缺口都位于**第 1 个与第 2 个音频包之间**（0 → 0.100 s），1250 帧完整。这是 ffmpeg 录制端在启动时对齐时间戳，与媒体流的中途中断无关。2026-11-05 已用确定性方法定位到机制：dump 中第一个音频 RTP 包和第一个音频 RTCP SR 只相隔约 20 µs，回放时分别发往两个 socket；ffmpeg 若先读到 RTP，第一帧就按未同步的时钟映射，下一帧起改按 SR 的时钟映射。把 SR 移到首包之后 50 ms 再回放，3/3 都报告 80 ms；移到首包之前 1 ms，3/3 都报告 0。见 [20261105-whep-probe](../20261105-whep-probe/report.md)。
  - D、B 的 dump 和 mkv 已被任务的大文件清理删除，因此“D/B 中的 80 ms 也是同一伪影”是根据数值和形态作出的 [推断]。

## 4. 真实 GB28181 摄像机：BLOCKED

只做了只读审查：

- 历史环境中的测试目录（`/var/tmp/gb-camera-e12340d-*`）仍在。
- 摄像机原地址可以 ping 通，TCP 80/554/37777 端口开放 [实测]。
- 被动抓包 75 s 没有收到摄像机发来的 SIP 报文，测试机上也没有 15060/5060 监听 [实测]。这与 [历史] 记录一致：测试结束后，槽位 1 已恢复为 `Disable`，指回原 SIP 服务器；槽位 0 注册在其他平台。

复核需要启用摄像机的平台槽位 1 并修改 SIP 服务器地址，这属于修改真实设备配置，本轮没有授权，因此没有执行。也没有使用旧账号或密码登录设备，没有用模拟器结果代替。

REGISTER/401/200、Keepalive、Catalog、INVITE/ACK/BYE、H.264+PCMA、Chrome WHEP、RTSP、多观看者、对讲、断网恢复、注销与重新注册，**本轮均未实测**。可以引用的只有 [20261023 e12340d](../20261023-e12340d-gb-camera/results.md) 的历史结果，其中观察到的首个 Keepalive SN=0、设备音频 PTS 不规则、注销等待 BYE 超时、状态更新时序、断网对讲清理提示，本轮无法确认当前状态。

## 5. 缺陷与修复

没有发现确定性生产缺陷：NO CHANGE。WHEP 探针的 80 ms 缺口已确认是测试工具的伪影（3.5 节），不修改生产代码；本轮也没有修改测试工具。工具已在 [20261105-whep-probe](../20261105-whep-probe/report.md) 中修复：音频连续性改按 dump 的 RTP 判定。

## 6. 未解决风险

- 真实摄像机兼容性没有在当前版本上复核；要复核，需要授权启用摄像机的独立平台槽位。
- WHEP av_probe 的启动对齐伪影会让 `audio_pts_gaps` 偶发误报。（2026-11-05：已改为按 dump 的 RTP 时间戳判定，录制侧的数值保留为 `recorded_audio_pts_*`，见 [20261105-whep-probe](../20261105-whep-probe/report.md)。）
- 控制面偶发 0.4–0.5 s 的尖峰（每 30 分钟 2–4 次）。它不影响媒体推进，也没有错误。（2026-11-05：最大的 3 次已归因到测量进程 runner 的停顿，其余 3 次原因不明，见 3.2 节。）
- 长稳只覆盖 1800 s 和 600 s。以小时或天计的运行（例如 jemalloc 长期碎片化、64 位计数回绕）没有测量。
- 以上结果都在测试 netns 的回环链路上测得，不包含真实网络的丢包与抖动。RTSP AAC 在丢包时额外丢弃 AU 的问题见 [20261104-rtsp-aac-timeline](../20261104-rtsp-aac-timeline/report.md)。

## 7. 其他

- **SSH Git 认证**：本机 `id_rsa`（SHA256:24Sa…）与 GitHub 账户公开的 key 114462490 指纹一致，主机密钥校验通过，经 SOCKS 代理连接后服务端仍拒绝该 key。原因在 GitHub 侧（例如 key 被停用或需要重新验证），本地无法进一步确认：查询 key 状态需要 `admin:public_key` 权限，当前 token 没有。`https://github.com/gyl30/media_server.git` 可以用已有凭据访问。没有修改全局 SSH 配置。
- **原始结果**：测试机 `runs11/`（s1-*、s2-*、s3-*）。更正（2026-11-05）：s3 的 dump 和 mkv 原本作为探针伪影的证据保留，但在 2026-11-05 09:24 被删除。删除者是之后提交的 `p01` 作业：它引用的旧作业库在启动时会运行 `cleanup.sh runs8`，删掉 `runs*` 下除 `runs8` 以外所有大于 1 MB 的媒体文件。现在只剩各 run 的 JSON、日志和本报告中的数值；剩余 dump 已复制到 `runs*` 之外，见 [20261105-whep-probe](../20261105-whep-probe/report.md)。
