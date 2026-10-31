# 性能工具与测量

本文件说明保留工具的职责和使用条件，不提供当前容量成绩。生产源码、工具 ELF、输入 fixture、运行环境和测量窗口共同决定结果；历史报告只对应当时的版本与配置。

## 工具清单

| 入口 | 用途 |
| --- | --- |
| `bench/perf_run.py` | 一次有授权、统一 readiness barrier、资源预算和有界清理的负载测量 |
| `bench/perf_capacity.py` | 驱动 perf_run 递增、二分并重复确认健康/失败边界 |
| `bench/perf_publish.py` | 通过真实 Go 信令创建发布源、发布换代并导出 sources.json |
| `bench/perf_validate.py` | 对正式测量框架做无效/过期/重放票据、源停止及取消负对照；不产生容量数据 |
| `bench/perf_profile.py` | CPU record/stat、off-CPU、已有 jemalloc dump 报告 |
| `bench/package_runtime.py` | 打包媒体服务、客户端、Python 工具、fixture 和动态依赖 |
| `bench/hls_client.py` | HLS viewer 网络负载 |
| `gb_ps_fanout`、`bench/gb_ps_fanout.py` | 共享 PS 封装微基准，不代表网络吞吐或解码 |
| `rtmp_fanout`、`rtsp_fanout`、`http_flv_fanout`、`whep_fanout`、`gb_fanout` | 原生协议客户端；对应源码在 `bench/` 和 `bench/clients/`。`gb_fanout` 通过控制接口创建 GB sender，按 UDP/TCP active/TCP passive 接收 RTP/PS，结束时删除自己创建的 sender |
| `rtsp_publisher`、`whip_publisher` | 原生发布客户端 |

同时保留独立负载入口：`performance_baseline.py`、`publish_only.py`、`mixed_fanout.py`、`rtmp_publish.py`、`rtsp_publish.py`、`whip_publish.py`、`rtmp_fanout.py`、`rtsp_fanout.py`、`http_flv_fanout.py`、`hls_fanout.py`、`whep_fanout.py`、`gb_network.py`（均位于 `bench/`）。它们各有进程、地址和窗口约定，不与正式 runner 的结果直接混合；部分使用旧本机固定流名，运行前需核对当前授权条件。

`fanout_support.py`、`perf_support.py` 和 `signaling_stub.py` 是共享实现。授权夹具直接位于 `bench/`，正式工具与运行包均不再依赖测试目录。旧生命周期、浏览器/UI 协议验证脚本及断连探针已删除；本轮没有运行负载或重新建立测试套件。

## 条件与一次测量

按 [构建部署](build-deploy.md) 启用 `MEDIA_SERVER_BUILD_BENCHMARKS`。运行环境需要 Linux `/proc`、Python ≥3.10、`bench/requirements.txt` 中的 aiohttp、taskset、FFmpeg/ffprobe，以及准备好的 H.264/AAC fixture。WHIP 使用离线准备的 H.264/Opus fixture。原生媒体健康检查不等同浏览器解码或实际声画验收。

将 DUT 与发布/播放客户端放在可达且隔离的地址空间，服务端和客户端 CPU affinity 不得重叠；确认端口、FD、磁盘和内存预算。默认地址是 `198.19.0.10/11`，不会由 runner 自动配置。可在专用实验网络 namespace 中给 loopback 配置这两个地址，再在该 namespace 内运行。不要复用正在服务业务的端口池或信令管理对象。

以下命令假设地址和 CPU 已准备好；FFmpeg 路径、fixture、核号按环境替换，输出目录必须不存在：

```sh
python3 bench/perf_run.py \
  --output /tmp/media-perf-run-new --input rtmp --outputs rtmp:10 --sources 1 \
  --server-bin build-perf/media_server --client-dir build-perf \
  --ffmpeg-bin /path/to/ffmpeg --ffprobe-bin /path/to/ffprobe \
  --fixture /path/to/h264-aac.flv \
  --host 198.19.0.10 --client-address 198.19.0.11 \
  --workers 4 --server-cpus 0-3 --client-cpus 4-7 \
  --warmup 15 --duration 60 --ready-timeout 60 --progress-interval 5 \
  --ramp-per-second 50 --min-free-memory-mib 1024 \
  --min-free-disk-mib 4096 --max-server-pss-mib 4096
```

outputs 支持 `rtmp/rtsp/flv/hls/whep/gb_udp/gb_tcp_active/gb_tcp_passive`，格式为逗号分隔的 `协议:数量`。GB TCP 输出和输入从 `--gb-port-base` 起为每个 sender/源占用一个端口；该范围与本机临时端口范围或服务端、上游实例的 RTMP/RTSP/HTTP 端口重叠时，runner 拒绝运行。`none` 仍保留每源持续 FLV viewer，仅为 PARTIAL publish-only，不能称零输出。

input 参数包含 RTMP、RTSP TCP/UDP 发布、WHIP、RTSP/RTMP/WHEP 拉流、GB UDP/TCP 和 existing。拉流与 GB 输入先在客户端核上启动一个上游实例，再通过 `POST /pull/create` 或 GB sender/receiver 接口接入被测服务。参见 [`perf_run.py`](../bench/perf_run.py) 与 [`pull_http.cpp`](../media/http/pull_http.cpp)。

`--inputs rtmp:24,rtsp_udp:8,whip:4,whep_pull:4,...` 在一次运行中混合多种输入，替代 `--input`/`--sources`；拉流与 GB 源按 `--upstream-sources` 共享上游流，`source_inputs` 记录各源的输入类型。上游实例与客户端共用地址时，其 WebRTC 媒体端口池（49152–65535）会与客户端占用的临时端口冲突，WHEP Pull 创建以 500 失败；混合负载应在测试命名空间内把 `net.ipv4.ip_local_port_range` 设为低于 49152 的范围。

`--fixture` 可以是一路 H.264/H.265 视频加一路 AAC/Opus/G.711 音频；客户端与输出协议是否支持对应编码需另行确认。`--decode-probe` 在测量窗口内用 ffprobe 对 RTMP/RTSP/FLV/HLS 输出各解码 6 秒，结果写入 `decode_probe` 并参与判定。`--av-probe N` 同时让第一组 WHEP 的第一个观众用 `--dump-file` 记录解密后的 RTP/RTCP，窗口结束后由 [`whep_dump_av.py`](../bench/whep_dump_av.py) 按原节奏回放给 ffmpeg 录制，再用 av_probe 分析，结果写入 `av_probe.whep`。回放端按 RTCP SR 对齐两条轨，因此测到的是浏览器按 SR 做唇音同步时看到的偏差。视频 RTP 时间戳存在 B 帧重排时，回放录制改为重新编码视频，避免 copy 封装因 DTS 非单调失败。GB 输出由每种传输第一组的第一个收到媒体的观众用 `gb_fanout --dump-file` 记录 PS 负载（RTP 序号已逐包校验连续），窗口结束后交给 av_probe 解封装解码，结果写入 `av_probe.gb_udp` 等；源音频为 G.711μ 时只解码视频并标注 `audio_unverified`，因为 FFmpeg 的 MPEG-PS 解复用器只在 IMKH 文件里识别流类型 0x91。`--whep-opus-fmtp` 让 WHEP 观众在 offer 中使用指定的 Opus fmtp（`whep_fanout --opus-fmtp`，默认 `minptime=10;useinbandfec=1;stereo=1`），例如 Chrome 默认的 `minptime=10;useinbandfec=1` 会使 WHIP 源走 AAC→Opus egress 而不是原始 Opus 直通。

自管模式启动自己的授权夹具和媒体进程。`--input existing` 必须提供 `--sources-file`、`--signaling-url`、`--server-pid`、`--control-token`，并使用匹配的 `--server-bin`；它只停止自己创建的客户端，不停止外部 DUT。sources.json 每项含 `stream_id` 和 `play_body`（其中指定 source_id、push_device_id 或 live_id）；声明 publisher PID 时还须提供 publisher_starttick。真实发布入口 `perf_publish.py --help` 给出生成这些数据的参数。

## 结果解释与容量

所有 viewer 就绪后 runner 写入 `measurement.start`，共享单调时钟窗口，并分别采样 server、publisher、client、runner 和系统资源。输出目录包含日志、凭据、命令及 `result.json`，按私有数据保存。

每组需要正常退出、ready/progressing 等于请求数，且实际窗口覆盖率至少 0.99；同时检查服务端退出、队列、UDP drops、客户端资源与清理。结果可能是 PASS、FAIL、INCONCLUSIVE、BLOCKED 或 UNSUPPORTED，必须同时读取 `capacity_boundary`、errors 和 cleanup，不能只看总字节数。

progressing 只表示每个进度间隔内收到过音视频。服务端 CPU 饱和时帧会在内部队列中累积，viewer 仍然 progressing，但媒体逐渐落后于实时。原生客户端因此按视频媒体时间戳在窗口内的推进量除以窗口时长，输出 `viewer_realtime_min/p10/p50/max`。窗口内服务端核合计繁忙率达到 90%、单核达到 98% 或服务端单线程达到 0.95 核时视为饱和；饱和且任一组最小值低于 `--min-realtime-ratio`（默认 0.98）或已经失败，判为 `SERVER_SATURATED`，未饱和时的落后判为 FAIL/`UNCONFIRMED`。原生客户端同时输出 `viewer_lag_span_ms_p50/p90/max`：每个 viewer 在窗口内“墙钟减视频媒体时间”的最大值与最小值之差。服务端靠合并写出维持实时比例时，它仍会随积压增大。准备阶段的就绪超时按失败处理，不归为环境阻断；播放票据有效期为 `--ready-timeout` 加 60 s。准备阶段失败时，如果服务端核在某秒的合计繁忙率达到 95% 或单核达到 98%，且客户端核未饱和、失败原因不是客户端资源或启动，也判为 `SERVER_SATURATED`。授权失败只有在签名桩没有拒绝、最长处理时间低于 1 s 时才视为服务端侧（服务端等待鉴权有截止时间），否则视为工具问题。`perf_capacity.py` 把 `SERVER_UNSTABLE` 与 `SERVER_SATURATED` 都作为失败边界。

`cpu_busy` 记录窗口内服务端核、其 SMT 兄弟核和客户端核在 `/proc/stat` 中的繁忙核数与 softirq，以及服务端核的 APERF/MPERF 平均工作频率、C0 驻留率和每秒周期数（需要 root 读取 `/dev/cpu/*/msr`）。调频策略会随负载改变频率，低负载下的 CPU 时间不能直接按线性外推到饱和点，比较成本时应结合周期数。`--perf-stat` 在窗口内对服务端核运行 `perf stat`，把 cycles、instructions、ref-cycles、page-faults、context-switches 和 IPC 写入 `perf_stat`；指令数不随频率变化，适合跨负载比较。

进程 CPU、PSS 和 FD 每秒采样，套接字队列每 5 次采样读取一次，避免 runner 在上万连接时自身成为瓶颈；runner 的有效采样少于窗口秒数的 80% 时结果为 INCONCLUSIVE/`RUNNER_RESOURCE`。`namespace_tcp_delta` 是窗口前后 namespace 内 TCP/UDP 重传、错误和 listen 溢出计数的差值；`verification` 记录签名桩的调用数、拒绝数和处理耗时 p99/最大值。

`--churn PROTOCOL:COUNT` 在窗口内每 `--churn-period` 秒（不少于 12，默认 20）启动一组额外 viewer，停留周期减 8 秒后退出，用于观察会话反复建立和回收；任一轮失败则整次失败，HLS 不支持。`--source-churn N`（不少于 10 秒）在窗口内每 N 秒新建一路 RTMP 发布，存在上游实例时再新建一路 RTSP 拉流，记录就绪耗时，半个周期后杀掉发布者并删除拉流，记录源消失耗时；任一轮失败则整次失败。`--source-churn-input whip` 改用 `whip_publisher` 发布，偶数轮 SIGTERM（发 WHIP DELETE）、奇数轮 SIGKILL（依赖 ICE 活动超时）。`--source-churn-viewers PROTOCOL:COUNT,...` 在每轮发布就绪后为该源启动这些观众组，前一半组在停止发布前退出，其余组须在源结束后 15 秒内退出，并记录观众加入前后的服务端 FD。`--cleanup-settle N` 在清理完成后等待 N 秒再取一次服务端快照（`cleanup.server_settled`），用于核对会话、FD 和内存是否回收。

容量入口示例：

```sh
python3 bench/perf_capacity.py --output /tmp/media-capacity-new \
  --protocols rtmp --start 100 --step 100 --granularity 25 --maximum 1000 \
  --healthy-repeats 3 --failed-repeats 2 --round-timeout 900 \
  -- --input rtmp --sources 1 --warmup 15 --duration 60 \
  --server-bin build-perf/media_server --client-dir build-perf \
  --ffmpeg-bin /path/to/ffmpeg --ffprobe-bin /path/to/ffprobe \
  --fixture /path/to/h264-aac.flv \
  --workers 4 --server-cpus 0-3 --client-cpus 4-7
```

driver 只用 HEALTHY 与 SERVER_UNSTABLE 建立边界，其他原因属于未决结果；到达 maximum 只证明所测上限。每轮 runner 退出后，driver 最多等待 60 s 让该轮进程组全部退出，超时强制结束，避免残留服务端占用端口污染下一轮。`--plan-only` 不运行负载。比较前后版本时固定硬件、CPU、依赖、输入码流、网络与窗口，分别保存服务端和客户端 ELF SHA；避免与构建、其他负载或采样同时争抢资源。

## Profiling

对已有 DUT 显式给 PID 和 `/proc/<pid>/stat` 的 starttime（第 22 字段），避免 PID 复用。输出目录须不存在：

```sh
python3 bench/perf_profile.py --mode cpu-record \
  --pid "$DUT_PID" --expected-starttick "$STARTTICK" --duration 30 \
  --output /tmp/media-profile-new --perf-bin perf
```

其他模式：`cpu-stat` 采集 CPU/调度/缺页计数；`offcpu` 配 `--offcpu-bin offcputime-bpfcc`；`heap-report` 配 `--binary`、`--heap-file` 和 `--jeprof-bin`，只读取已经生成的非空 jemalloc dump，不启用 profiling 或生成 dump。

perf/BCC/jeprof 及权限由运行环境提供。无样本、缺失计数、工具缺失或身份不匹配要保留 BLOCKED 证据，不能记为零开销。采样结果是诊断证据，需要单独做采样开销对照；热点、RSS 或一次运行不能独立证明优化收益、泄漏或最大容量。

## 运行包与本地证据

```sh
python3 bench/package_runtime.py --output /tmp/media-runtime-new \
  --build-dir build-perf --fixture /path/to/h264-aac.flv \
  --ffmpeg-bin /path/to/ffmpeg --ffprobe-bin /path/to/ffprobe \
  --whip-fixture /path/to/h264-opus.mkv --signaling-bin build-go/signaling
```

后两项可选。工具收集所需 ELF/库、Python 文件及 NSS 模块，输出 manifest 和 `run.sh`；不包含浏览器、模拟器、profiler 或宿主系统运行库。`manifest.head` 表示打包源码树，`binary_source_head` 当前为 NOT_RECORDED，不能据此推断 ELF 的编译提交；应另存构建日志和校验和。

`docs/performance_reports/`、`docs/verification_results/` 只跟踪报告与脚本；其中的原始数据、本地 build 和原始采样文件由实际运行者管理，未跟踪/忽略内容不随文档清理删除。历史记录缺少环境或原始数据时，不补造结果，也不转写成当前成绩。各报告的版本、结论、与当前代码的关系和缺失引用见 [历史报告导航](performance_reports/README.md)。
