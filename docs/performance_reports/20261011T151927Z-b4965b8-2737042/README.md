# media_server 性能报告：20261011T151927Z-b4965b8-2737042

## 结论

- 整体：**PARTIAL**。Stage 0（双机预检、构建、打包、部署）完成；**Stage 1 协议 smoke 8/8 PASS**；Stage 2 及以后 `NOT_RUN`。
- 本轮**所有性能与功能测量都在 TEST（<TEST_HOST>）执行**；DEV 只做 Git worktree、编译、打包与归档。
- 版本：`BASE_HEAD = b4965b8a928ed692492db2f176cd96f9a0759b78`，worktree 干净，未修改任何媒体业务代码。
- 构建：`ctest` **55/55 通过**；`media_server` 等 7 个 ELF 的 SHA256 见 `manifest.json`，与 TEST 上部署后的实测 SHA256 一致。
- 运行包：`runtime.tar.gz` SHA256 `79b992352e2beb1ef84bd20ea168085fa6fc92b23fcbe4f0ad259d44522e7237`（201 099 364 B），TEST 侧 `sha256sum -c` 通过、`ldd` 无 `not found`。
- 媒体：真实摄像机（`<RTSP_CAMERA_HOST>:554`）无转码直录 → 离线一次转标准 profile。SHA256 见下；凭据未进入任何日志、命令记录或报告。
- CPU：TEST 上 `SERVER_CPUS=0,1,2,3,4,5,6,7`（物理核 0-7，每核一个超线程，sibling 16-23 不分配）、`CLIENT_CPUS=8,9,10,11,12,13,14,15`（物理核 8-15，sibling 24-31 不分配），`--threads 8`。
- 网络：一次性 namespace `msperf-20261011T151927Z-b4965b8-2737042`，`lo` 上 `198.19.0.10`（服务端）/`198.19.0.11`（客户端），无默认路由；既有 `media-test` namespace 未触碰。
- 首个瓶颈：**未确认**。1 观众档位下服务端 CPU 0.005-0.062 核，远未饱和。

## 环境与版本边界（两台机器分开记录）

| 项 | DEV <DEV_HOST> | TEST <TEST_HOST> |
|---|---|---|
| 角色 | worktree、编译、打包、归档 | 全部测量、namespace、affinity、profiler |
| 硬件 | i7-13700KF / 31 GiB（**不作为测试配置**） | Xeon Silver 4216 16C/32T、单 NUMA、93 GiB、空闲 |
| 内核 | 6.8.0-136-generic | 6.8.0-84-generic |
| 工具链 | CMake 3.31.10、GCC 16.0.1 exp、Boost 1.92 静态、FFmpeg SDK 9.0.1（`libavcodec 63.1.101`，必须 `PKG_CONFIG_PATH=/home/gyl/ffmpeg901/lib/pkgconfig`） | perf 6.8.12（root 可用）、bpftrace v0.14.0、offcputime-bpfcc（依赖链不完整）、bundled ffmpeg n7.1.1-57 |
| 变更 | 仅新增 worktree 与临时目录 | 安装 `aiohttp 3.14.5`（pip3）、`python3-numba`（apt）；未改 sysctl/governor/全局 ulimit |

`kernel.perf_event_paranoid=4` 与 `kptr_restrict=1` 在两台机器上都存在，但 TEST 以 root 运行，`perf stat`/`perf record` 实测可用；DEV 的无 root 限制不构成 TEST 的结论。

## 固定素材

| 阶段 | 文件 | SHA256 | 媒体 |
|---|---|---|---|
| 摄像机直录 | `camera_native.mkv` | `423ad4b5dec3cb8a6dcc46f0b6cb3abafcfd17693b393854c7181faeb17b1ce5` | HEVC 2560x1440@25 + G.711A 8k mono，120.033 s，4.04 Mbps，`-c copy` |
| 标准派生（正式基准用） | `camera_standard.flv` | `59c81471062a78e114205c96c03a99a1a3f432e50365bcd587edccd8ba029741` | H.264 Constrained Baseline L3.1 1280x720@30 CFR、2.0 Mbps CBR、GOP=60、AAC-LC 48k stereo 128k，120.054 s |

派生参数与 `ffprobe` JSON 已随本轮临时目录保存；TEST 侧 `runtime/.cache/perf_fixture_720p30.flv` 的 SHA256 与上表一致。

## Stage 1 smoke 结果（TEST，1 source × 1 viewer，warmup 5 s / 测量 15 s，各 1 次）

| case | 状态 | 服务端 CPU 核 | PSS 中位 (KiB) | FD | 媒体健康证据 |
|---|---|---|---|---|---|
| `rtmp_publish` | PASS | 0.0060 | 20 303 | 33 | 窗口内与窗口末 HTTP-FLV 探测均 200 |
| `rtsp_publish_tcp` | PASS | 0.0060 | 21 087 | 33 | 同上 |
| `rtsp_publish_udp` | PASS | 0.0047 | 21 013 | 37 | 同上 |
| `rtmp_play` | PASS | 0.0118 | 21 283 | 34 | `progressing=1 failed=0`，视频 3 866 597 B，首媒体 p50 275 ms |
| `rtsp_play` | PASS | 0.0144 | 21 304 | 34 | `progressing=1 failed=0`，视频 3 928 787 B，首媒体 p50 314 ms |
| `flv_play` | PASS | 0.0140 | 21 586 | 34 | 4 084 860 B，16 个每秒增长采样 |
| `hls_play` | PASS | 0.0104 | 24 502 | 33 | 27 782 857 B，13 次播放列表轮次持续增长 |
| `whep_play` | PASS | 0.0615 | 28 634 | 34 | `progressing=1`，4 052 298 B / 6 768 包（视频 6007 + 音频 750），`unprotect_failures=0`，首媒体 p50 269 ms |

所有档位 `udp_drops=0`，窗口末源仍可读，客户端返回码 0。原始记录在 `runs/smoke-*/`（`result.json`、`server.log`、`client.log`、`publisher.log`、`measure.log`）。

## 工具与矩阵限制（重要）

1. **仓库 `bench/*.py` 已与现行鉴权模型脱节**：它们发布 `live/perf0`，而服务端要求信令签发的一次性 64-hex `stream_id/token`（`media/rtmp/rtmp_session.cpp:254`；`signaling/stream_token.go` 的 `consumeStreamToken` 成功后 `delete` 令牌）。直接运行会以 `NetStream.Publish.BadName` 失败。
2. 本轮在 **DEV worktree 新增 `bench/perf_smoke.py`**（并把 `tests/signaling_stub.py` 随包部署）作为符合现行模型的 smoke 驱动；**未改动媒体业务代码**。
3. **多观众能力缺口**：只有 `whep_fanout` 支持 `--whep-urls-file` 逐观众 URL；`rtmp_fanout`/`rtsp_fanout` 仅有单一 `--stream-name`，HTTP-FLV/HLS 客户端也没有逐令牌入口 → N>1 的 RTMP/RTSP/HTTP-FLV/HLS 容量测试需要先扩展这些客户端。
4. `whip_publisher` 只能自造流名（`--whip-base-url` + `--stream-prefix`）→ WHIP 输入 smoke 记 **BLOCKED**。
5. 所有 bench 脚本都没有 `--control-token`，而服务端在非回环地址要求 `Bearer` 令牌 → GB28181 接收/发送（走 `/gb28181/*`）记 **BLOCKED**。
6. 仓库无 RTSP 拉流基准工具（不调用 `/rtsp/pull/create`）→ RTSP pull 输入记 **BLOCKED**。
7. TEST 上 `offcputime-bpfcc` 依赖链不完整（安装 `python3-numba` 后仍缺 `pytest`）；`perf stat/record` 与 `bpftrace` tracepoint 实测可用。

## 下一阶段可直接执行

- 先决定第 3-6 项工具缺口的处理方式（扩展 native 客户端逐令牌入口 / 给 bench 脚本加 `--control-token` / 新增 RTSP pull 工具），否则 Stage 2 只能覆盖 RTMP 输入 → WHEP 输出这一条完整链路。
- 运行器已就绪：`$TEST_RUN/run_smoke.sh $TEST_RUN/env.sh`（TEST 侧），Stage 2 只需替换 `--warmup/--duration/--viewers` 与档位循环。

## 清理与边界

- 本轮每个 case 结束后由驱动停止 client/publisher/server；收尾时删除本轮 namespace 并确认 `ip netns pids` 为空、无遗留 `media_server`/`ffmpeg`/`perf_smoke` 进程。
- 既有 `media-test` namespace、DEV 上用户自己的 `build/media_server`（PID 246714）均未触碰。
- 主工作区未修改、未提交、未推送；`docs/performance_reports/` 由 `.git/info/exclude` 忽略。
- `NOT_RUN`：Stage 2 正常基准、Stage 3 容量搜索、Stage 4 过载诊断、Stage 5 长时验证、On-CPU/Off-CPU/jemalloc 专项。`BLOCKED`：WHIP 输入、GB28181 收发、RTSP pull 输入（原因见上）。
