## 9 工具补充与自验证

全部工具修改只涉及 `bench/`、`CMakeLists.txt`（新增 `gb_fanout` 目标）和 `docs/performance.md`，按问题分别提交，未推送：

| 提交 | 内容 | 自验证 |
| --- | --- | --- |
| `1338812` | 原生客户端按视频媒体时间输出 `viewer_realtime_*`，饱和判定；新增 `gb_fanout`（GB UDP/TCP active/TCP passive 输出）及全部输入类型、`--decode-probe` | 1 核负对照 `v1/v3/v4/v5/v6-neg-rtmp-1cpu-*` 判为饱和/落后；正常负载 `v1/v2-*` 全部 realtime ≥0.99 |
| `60a96c4` | runner 套接字队列改为每 5 次采样读一次、复用 PSS；客户端复用 64 KiB 读缓冲；`--churn` | RTMP 6500 原 runner 0.994 核、判 CLIENT_RESOURCE；修正后 runner ≤0.37 核（11000 连接）；`v8-churn-rtmp/gb` 通过 |
| `68a22ae` | 降频采样后 udp_drops 只读取含该字段的样本 | `v9-rtmp-500-d33`、`v9-gb_udp-300-d31` |
| `0418e40` | `--cleanup-settle` 清理后延迟快照 | 见第 8 节 |
| `6ef7c32` | 签名桩记录调用数、拒绝数、处理耗时 | 全部结果的 `verification` |
| `bb16e5c` | `--perf-stat` 统计服务端核 cycles/instructions | 与 APERF/MPERF 周期数逐轮比较，差异 −0.1%～+0.4% |
| `86052ae` | 签名桩零拒绝且最长处理 <1 s 时，准备阶段鉴权失败计入服务端饱和 | WHEP 3750 由 UNCONFIRMED 变为可判定 |
| `c1fd56e` | 清理遇到传输错误不再逐个超时；driver 回收每轮进程组；签名桩 backlog 4096 | `v11-stub-r400`：ListenOverflows 0、桩 p99 0.9 ms，无新的 SYN flooding；`v11-gb_udp-300` 清理 128/128 |
| `14eaecd` | 客户端退出时 `/proc/<pid>/exe` 先消失的竞态 | FLV 6500 原判 BLOCKED，重测见第 4 节 |
| `1c2ff7d` | GB TCP 监听端口与临时端口范围重叠时拒绝运行 | 10500 通过、11000 被拒；原 11000 两轮 `Address already in use` 为工具冲突，作废 |
| `4930da7` | `docs/performance.md` 补充上述选项与判定 | — |
| `49cf28b` | GB TCP 监听端口还须避开服务端与上游实例的 RTMP/RTSP/HTTP 端口 | `--gb-port-base 10000` 跑 2000 个 passive 时直接拒绝（端口 11935 落在范围内）；原 `cap2c-gb_tcp_passive` 因该冲突 create 失败（`14:Bad address`），作废 |
| `f3f5bfb` | 原生客户端记录每个 viewer 窗口内 `墙钟 - 视频媒体时间` 的最大值与最小值之差，输出 `viewer_lag_span_ms_p50/p90/max` | 轻载 100～145 ms（30 s 窗口，含发布端节拍和回环交付抖动）；1 核 GB TCP 1500/1800 为 162/180 ms；RTMP 8500 为 453～876 ms，WHEP 3500 为 863～1037 ms（3250 为 312～410 ms），随饱和单调增大 |
| `c64e7d8` | `--perf-stat` 增加 page-faults、context-switches | RTMP 6500 每秒 21 万次缺页，与 profile 中缺页栈占比一致；上下文切换每秒 40～660 次 |
| `862ee0c` | 就绪超时（`TimeoutError`，是 `OSError` 子类）不再归为 BLOCKED | `cap6c-whep-confirm/round-000-3750` 判为 FAIL/SERVER_SATURATED |
| `c775ca5` | 播放票据有效期由固定 60 s 改为 `--ready-timeout + 60` | 原 `cap6b-whep-confirm` 3750 一组建立 73 s，末尾 2 个 viewer 403；修正后 `cap6c` 两轮签名桩拒绝 0 |
| `cad6ddc` | `docs/performance.md` 补充 lag span、缺页/上下文切换统计、GB 端口检查、就绪超时与票据有效期 | — |
| `939c249` | 发布端改为同时发 SIGTERM、共用 10 s 等待后 kill | ffmpeg 7.1.1 在 RTMP 输出连接未断时不响应 SIGTERM，原来逐个等待 10 s，100 个上游发布端清理约 17 min；本地 7 个进程 10.0 s 内全部停止；测试机上 `032-multisource` 的 7 轮共 522 个 RTMP 发布端（含 300 源一轮），整个作业 12 min 完成（queue.log 08:25～08:37），按原逐个等待方式仅清理就要约 87 min |
| `551a61c` | `namespace_tcp_delta` 增加 TLP、RTO 超时、快速重传、backlog/接收队列/零窗口丢弃、prune 计数 | 之后全部轮次的重传都等于 `TCPLossProbes`，其余计数为 0（第 6.3 节） |
| `370de80` | churn 组改为日志出现 `media_ready` 后再写测量开始时间（原为启动后 ramp+3 s 预写） | `d1-long-whep-3000` 的 87 轮 churn 全部因 `start-file time is outside the readiness deadline` 失败：50 个 WHEP 会话在负载下约 7 s 才全部出媒体（`churn-0.log`）。修正后 `c1-whep-1000-churn`（WHEP 1000 + churn 50）5/5 轮、`c1-rtmp-1000-churn`（RTMP 1000 + churn 100）4/4 轮通过；WHEP 长稳用新运行包重跑（`d2-long-whep-3000`，第 8.2 节） |

外部工具：发布端和解码探测默认使用运行包内的 ffmpeg/ffprobe n7.1.1（`/home/gyl/bin`）。n7.1.1 的 FLV 封装拒绝 Opus（`Audio codec opus not compatible with flv`），它的 ffprobe 也无法识别 Enhanced RTMP 的 FourCC Opus 音频（codec 显示 `unknown`），而服务端 RTMP/HTTP-FLV 输出 Opus 时正是用这种格式。因此 `v4-codec-h264_opus-rtmp` 判为工具缺失。经用户同意，10-24 安装 BtbN 静态构建 ffmpeg n9.0.2-35-g9a9b70bb8c-20261023（本机 `/home/gyl/opt/ffmpeg-n9.0`，测试机 `/var/tmp/ms-perf-891aec1/ffmpeg-n9.0`；ffmpeg sha256 `efe9247b…e663`，ffprobe `e5ddfa26…4301`；下载包已用官方 checksums.sha256 校验），只用于含 Opus 的 `b2-codec-*` 格子：RTMP Opus 发布和 RTMP 拉流用 n9.0 的 ffmpeg 和 ffprobe，RTSP Opus 发布保持 n7.1.1 发布端，只把解码探测换成 n9.0。自验证：在测试机客户端核上用 n9.0 把 H.264/H.265 + Opus 夹具封装成 FLV，再用 n9.0 ffprobe 读出 120/119 个视频帧和 201 个 Opus 帧（4 s）。其他轮次和全部容量轮次的工具版本不变。

工具开销（容量轮次，`analysis/cap2-costs.csv` 及各 `result.json`）：runner 0.12～0.37 核；客户端单核最高 0.73、单线程最高 0.50 核；服务端 SMT 兄弟线程 0.004～0.035 核。没有一轮因客户端或 runner 资源受限。

报告目录 `tools/` 下的分析脚本（不提交）：`pr.sh`/`pc.sh`（netns 内运行 perf_run/perf_capacity）、`prof2.sh`（固定负载下 perf stat、IMC、softirq、perf record、syscount、offcputime）、`costs.py`、`capsum.py`、`profsum.py`、`stackcat.py`（按调用栈把周期归入发送系统调用、softirq、缺页、用户态各类）、`proftable.py`（汇总第 7.2 节分类表）、`matrix.py`（矩阵各组就绪与解码探测）、`sum.py`（单轮判定摘要）、`longsum.py`（长稳轮按 5 min 汇总 CPU、PSS、FD 和 churn）、`assemble.py`（把 `analysis/` 各节拼成 `report.md`）、`offcputime.py`（系统 `offcputime-bpfcc` 的副本，kprobe 改为内核 6.8 的 `finish_task_switch.isra.0`）。

作废数据（`analysis/invalid-rounds.txt`，`tools/capsum.py` 统计时剔除）：`cap2-flv-invalid`、`cap2-rtsp-invalid`、`cap2-gb_tcp_passive-invalid`（上一轮 GB UDP 8000 遗留的服务端占用端口），`cap2-rtsp-invalid2`（运行中替换运行包目录，触发身份校验），`cap2-gb_tcp_active` 的 11000 三轮（012/017/018，端口冲突），`cap2c-gb_tcp_passive-invalid`（端口与 RTMP 11935 冲突），`b2-codec-h264_opus-rtsp_pull`（任务脚本漏配 ffmpeg n9.0，上游无法以 FLV 推 Opus）。

分类修正但保留的轮次：`cap6-whep-confirm/round-003-3750`（原 BLOCKED，实际为就绪超时）和 `cap6b-whep-confirm/round-003/004-3750`（混有票据过期的 403，同时有 STUN 超时），三轮按 FAIL 计。服务端饱和的事实不受工具问题影响，修正后的 `cap6c` 两轮也给出同样结论。

报告目录还有 `tools/envlog.sh`（10-24 05:15～15:05 在 CPU 31 上运行，`nice 10`，测试结束后已停止），每 5 s 记录 uncore 频率、封装/DRAM 功耗、服务端核平均频率和封装温度，写入测试机 `envlog.tsv`（副本在 `raw/env/`），用于核对时段漂移（第 6 节）和功耗、温度（第 2 节）。
