# 性能与容量基线

本轮使用独立 RelWithDebInfo 构建，`-O2 -g -DNDEBUG`，无 Sanitizer，6 个 server workers。环境、fixture hash 与稳定性验证见 [stability_verification.md](stability_verification.md)。测量在本轮其他构建和压力测试结束后串行进行。

```bash
export PKG_CONFIG_PATH=/tmp/libsrtp-2.7-prefix/lib/pkgconfig:/home/gyl/ffmpeg901/lib/pkgconfig
cmake -S . -B build_perf -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DMEDIA_SERVER_BUILD_BENCHMARKS=ON
cmake --build build_perf -j12
python3 bench/performance_baseline.py --build-dir build_perf \
  --fixture h264-aac.flv --warmup 10 --duration 30 --runs 3 \
  --whep-levels 1 100 250 500 1000 --output perf/baseline
```

每个 workload 先等待实际首媒体，再 warmup 10 秒，测量 30 秒，至少三次。CPU 为进程 CPU seconds / wall seconds，单位 cores；RSS 每次取约 1 秒采样的 median，再对三次运行取 median；吞吐、first-media p50 和失败数也取三次 median。每次原始 `result.json`、客户端日志、命令、server/publisher 日志都保留，汇总为 `summary.json`。

首媒体定义有区别：publish-only 从发布进程启动到临时 HTTP-FLV probe 收到媒体 tag（probe 在 warmup 前关闭）；RTMP 排除 codec config，以实际视频计量；RTSP 以 RTP 媒体计量；HTTP-FLV 在成熟 HTTP 解 chunking 后解析实际媒体 tag；HLS 等待并读完首个媒体分片；WHEP 从 POST 开始到视频和 Opus 音频均收到并解保护成功。不同协议的首媒体延迟不能直接等同播放器出画延迟。

吞吐为媒体边界的接收字节，不是统一的网卡 wire bytes：publish-only 是 FFmpeg FLV 输出 total_size 增量；RTMP 是 coded FLV audio/video payload；RTSP 是 RTP callback 字节；HTTP-FLV 是解 chunk 后的 FLV body；HLS 是分片 body；WHEP 是解保护后的 clear RTP/RTCP 字节，以实际测量时长计算。各协议 header、封装、采样边界不同，所以不应把吞吐小差异直接解释为丢帧。

HLS 沿用现有客户端的具体负载：按 playlist target duration 轮询，每轮只请求最新分片，会跳过中间分片。因此表中 HLS 吞吐是该轮询策略的实际下载量，不能当成完整顺序播放的吞吐或将其低于输入码率解释为服务丢帧；完整播放器的双轨解码由稳定性套件中独立 FFmpeg 验证。本轮没有将基准客户端扩展为完整 HLS 播放器。

## WHEP 容量

使用同一真实 H.264+AAC fixture，覆盖共享 AAC→Opus egress。依次尝试 100 / 250 / 500 / 750 / 1000 viewers，每级独立服务；8 client IO threads，ramp 100/s，warmup 5 秒，measurement 20 秒。

```bash
python3 bench/whep_fanout.py --server-bin build_perf/media_server \
  --client-bin build_perf/whep_fanout --fixture h264-aac.flv \
  --workers 6 --client-threads 8 --ramp-per-second 100 \
  --viewers 100 --warmup 5 --duration 20 --output perf/capacity-100
```

`ready` 表示 POST+ICE+DTLS 建立成功，`media_ready` 表示双轨首媒体，`progressing` 表示测量窗口内双轨都有增量。通过要求全部 viewers 满足三项、无 establishment/runtime/SRTP unprotect failure、正常停止并成功删除全部资源、无 UDP drops 或 write queue full。

UDP queue/drop 从该 server PID 拥有的 socket inode 统计，排除 network namespace 中其他进程的 socket。外层监控只保护本次启动的服务，RSS 达到 2 GiB 时终止该服务并停止继续升压；正常完成后若服务 CPU 达到 6 workers 的约 90%（5.4 cores），也停止继续增加。容量边界不能等同生产 correctness 缺陷，不能以触发 OOM 为目标。

五个容量级别全部通过。每级 ready/media_ready/progressing、stopped/removed 均等于请求 viewers；establishment/runtime/SRTP unprotect failures、UDP drop、write queue full 均为 0，server 线程均为 6。

| Viewers | CPU cores | RSS median KiB | PSS median KiB | peak RSS KiB | FD median | Gbit/s | 首媒体 p50 / p95 / p99 / max ms | UDP TX / RX queue max bytes |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- | --- |
| 100 | 0.3624 | 58,278 | 46,468.5 | 58,920 | 127 | 0.8011 | 505 / 954 / 993 / 1003 | 4608 / 0 |
| 250 | 0.8659 | 100,144 | 88,856 | 106,668 | 277 | 2.0038 | 414 / 962 / 1013 / 1045 | 6912 / 0 |
| 500 | 1.7726 | 188,600 | 178,281 | 201,736 | 527 | 4.0124 | 515 / 964 / 1015 / 1055 | 11520 / 960 |
| 750 | 2.6805 | 274,206 | 262,665.5 | 289,208 | 777 | 6.0185 | 483 / 967 / 1002 / 1031 | 5120 / 0 |
| 1000 | 3.7600 | 360,404 | 349,107 | 394,440 | 1027 | 8.0135 | 510 / 971 / 1017 / 1058 | 6912 / 960 |

停止原因：达到本轮预定 1000 viewers 上限，未触发 RSS/CPU/丢包/队列停止条件。**1000 是本次验证过的负载，不是已经找到的最大容量**。每级只有本次 20 秒窗口，长期稳定性边界另见 soak，不将两者混为同一负载证明。

三轮性能选择 1 / 100 / 250 / 500 / 1000 WHEP viewers；包括 publish-only 和其他四协议各 1/4/8，共 18 workloads、54 次运行。全部 54 次运行通过，失败计数均为 0。

| Workload | median CPU cores | median RSS KiB | median Mbit/s | median 首媒体 p50 ms | failures |
| --- | ---: | ---: | ---: | ---: | ---: |
| publish_only-1 | 0.0023 | 21,684 | 7.820 | 764.7 | 0 |
| rtsp-1 | 0.0050 | 22,722 | 7.906 | 460.0 | 0 |
| rtsp-4 | 0.0130 | 24,332 | 31.622 | 422.0 | 0 |
| rtsp-8 | 0.0230 | 24,340 | 63.308 | 380.0 | 0 |
| rtmp-1 | 0.0047 | 22,352 | 7.803 | 462.0 | 0 |
| rtmp-4 | 0.0087 | 24,026 | 31.212 | 421.0 | 0 |
| rtmp-8 | 0.0170 | 25,030 | 62.425 | 382.0 | 0 |
| http_flv-1 | 0.0043 | 22,174 | 7.811 | 461.6 | 0 |
| http_flv-4 | 0.0110 | 23,046 | 31.246 | 462.4 | 0 |
| http_flv-8 | 0.0180 | 23,808 | 62.492 | 461.3 | 0 |
| hls-1 | 0.0030 | 39,676 | 5.927 | 2505.0 | 0 |
| hls-4 | 0.0033 | 39,524 | 23.709 | 2405.0 | 0 |
| hls-8 | 0.0040 | 39,636 | 47.418 | 2404.0 | 0 |
| whep-1 | 0.0137 | 32,392 | 8.025 | 459.0 | 0 |
| whep-100 | 0.3627 | 57,674 | 802.514 | 499.0 | 0 |
| whep-250 | 0.8946 | 99,878 | 2007.208 | 419.0 | 0 |
| whep-500 | 1.7910 | 183,280 | 4013.199 | 513.0 | 0 |
| whep-1000 | 3.7602 | 379,504 | 8027.526 | 502.0 | 0 |

测量代码/工具 HEAD：`8af98842f69d3e63a7e2d95b29f454d606992412`。三次原始值和环境记录见 [performance_baseline.json](verification_results/performance_baseline.json)。原始单次命令与日志位于 `/tmp/media_server_verify-728178f/performance-baseline/<workload>/<run>/`。

## 历史比较

全部 refs 中没有找到历史 `docs/performance_baseline.md` 或旧脚本的真实测量数值。旧 `tests/performance_baseline.sh` 在 `8f3260d` 引入、`c055cc3` 增加线程采样、`4f9c5dd` 删除；完整读取方式：

```bash
git show 4f9c5dd^:tests/performance_baseline.sh
```

旧脚本也是 warmup 10 秒、measurement 30 秒、3 runs，以及 RTMP/RTSP/HTTP-FLV/HLS 的 1/4/8 viewers，fixture 是 720p30 H.264 ultrafast + 44.1 kHz stereo AAC 96k。本轮沿用这些测量窗口和工作负载，复用当前公开接口，没有恢复旧 signaling allocation API 或旧服务参数。

[whep_shared_rtp_experiment.md](whep_shared_rtp_experiment.md) 保留了 WHEP 实验控制组数值：500 viewers，6 workers，8 client IO threads，ramp 100/s、60 秒窗口，CPU 2.228 cores、PSS 200,404 KiB、1.1134 Gbit/s、queue full 0；约 2.23 Mbit/s/viewer。本轮 fixture 约 7.819 Mbit/s，测量窗口为 20 秒容量 / 30 秒基线；编译器、kernel、完整环境和 fixture hash 也没有对应的历史同条件数据。

因此本轮与历史 **不可直接做百分比比较，也不能据此宣称性能 regression**。当前数据建立新的可复现基线。RSS 与 PSS 分开记录，旧实验的控制组和已经否决的共享 RTP 候选不能混合比较；本轮没有恢复该候选。
