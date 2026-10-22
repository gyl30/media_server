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
| `bench/hls_client.py`、`bench/gb_sender_fanout.py` | HLS viewer 与 GB sender 网络负载 |
| `gb_ps_fanout`、`bench/gb_ps_fanout.py` | 共享 PS 封装微基准，不代表网络吞吐或解码 |
| `rtmp_fanout`、`rtsp_fanout`、`http_flv_fanout`、`whep_fanout` | 原生协议客户端；对应源码在 `bench/` 和 `bench/clients/` |
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

outputs 支持 `rtmp/rtsp/flv/hls/whep/gb_udp/gb_tcp_active/gb_tcp_passive`，格式为逗号分隔的 `协议:数量`。`none` 仍保留每源持续 FLV viewer，仅为 PARTIAL publish-only，不能称零输出。

input 参数包含 RTMP、RTSP TCP/UDP 发布、WHIP、GB UDP/TCP 和 existing。`rtsp_pull` 通过 `POST /pull/create` 创建主动 RTSP 拉流；接口路径已修正，但该模式尚未完成实际运行验证。参见 [`perf_run.py`](../bench/perf_run.py) 与 [`pull_http.cpp`](../media/http/pull_http.cpp)。

自管模式启动自己的授权夹具和媒体进程。`--input existing` 必须提供 `--sources-file`、`--signaling-url`、`--server-pid`、`--control-token`，并使用匹配的 `--server-bin`；它只停止自己创建的客户端，不停止外部 DUT。sources.json 每项含 `stream_id` 和 `play_body`（其中指定 source_id、push_device_id 或 live_id）；声明 publisher PID 时还须提供 publisher_starttick。真实发布入口 `perf_publish.py --help` 给出生成这些数据的参数。

## 结果解释与容量

所有 viewer 就绪后 runner 写入 `measurement.start`，共享单调时钟窗口，并分别采样 server、publisher、client、runner 和系统资源。输出目录包含日志、凭据、命令及 `result.json`，按私有数据保存。

每组需要正常退出、ready/progressing 等于请求数，且实际窗口覆盖率至少 0.99；同时检查服务端退出、队列、UDP drops、客户端资源与清理。结果可能是 PASS、FAIL、INCONCLUSIVE、BLOCKED 或 UNSUPPORTED，必须同时读取 `capacity_boundary`、errors 和 cleanup，不能只看总字节数。

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

driver 只用 HEALTHY 与 SERVER_UNSTABLE 建立边界，其他原因属于未决结果；到达 maximum 只证明所测上限。`--plan-only` 不运行负载。比较前后版本时固定硬件、CPU、依赖、输入码流、网络与窗口，分别保存服务端和客户端 ELF SHA；避免与构建、其他负载或采样同时争抢资源。

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

`docs/performance_reports/`、`docs/verification_results/`、本地 build 和原始采样文件由实际运行者管理，未跟踪/忽略内容不随文档清理删除。历史记录缺少环境或原始数据时，不补造结果，也不转写成当前成绩。
