# perf_run：授权、媒体进展与统一测量窗口

入口：`python3 bench/perf_run.py --help`。这是现有协议客户端的工作负载编排器，不是解码器，也不自动创建 namespace 或调整系统设置。

## 依赖与模式

- Python 3、Linux `/proc`、`taskset`；服务端和客户端 CPU 集必须不相交。
- `media_server`、所选 native fanout/WHIP 二进制、FFmpeg、ffprobe。HLS 和 GB sender 客户端是 Python 脚本，依赖随运行包说明检查。
- 自管模式使用现有 `SignalingStub` 一次性验证服务，不等同真实 Go signaling E2E。输入素材必须有 H.264 视频和 AAC 音频；WHIP 另需 H.264/Opus fixture。
- `existing` 模式使用已经运行的服务及真实 signaling。工具不停止该服务、不删除外部 receiver、不停止外部 publisher。

支持的输入：`rtmp, rtsp_tcp, rtsp_udp, rtsp_pull, whip, gb_udp, gb_tcp_active, gb_tcp_passive, existing`。

支持的输出：`rtmp, rtsp, flv, hls, whep, gb_udp, gb_tcp_active, gb_tcp_passive`。`--outputs none` 是 **PARTIAL publish-only**：每个源仍有一个 FLV 双轨客户端持续运行整个稳态窗口，**不是严格零 viewer，也不是只运行一次的首媒体探针**。其客户端和服务端输出开销必须保留，不能当作纯发布成本。

本工具总会准备媒体源，没有 idle 模式。严格空载或零输出发布基线需要独立 DUT/发布者和同长度的资源采样，不能用本工具结果替代。

## CLI

| 参数 | 默认/要求 |
|---|---|
| `--output` | 必填，新目录；已有目录拒绝覆盖 |
| `--input` / `--outputs` / `--sources` | `rtmp` / `rtmp:1` / `1`；输出使用逗号分隔 `协议:数量`，不可重复 |
| `--sources-file` | existing 的 JSON：`[{"stream_id":"...","play_body":{"push_device_id":"..."}}]`；也支持现有 `source_id` 或 `live_id` 来源；含 `publisher_pid` 时必须提供 `publisher_starttick`，由新版 perf_publish ready 后生成 |
| `--signaling-url` / `--server-pid` / `--control-token` | existing 必填，后两者为已有服务 PID 和控制凭据；自管模式不填外部来源字段 |
| `--server-log` | existing 服务日志，仅观察，不修改 |
| `--server-bin` / `--client-dir` | `build/media_server` / `build` |
| `--ffmpeg-bin` / `--ffprobe-bin` | `bin/ffmpeg` / `bin/ffprobe` |
| `--fixture` / `--whip-fixture` | `.cache/perf_fixture_720p30.flv` / `.cache/perf_fixture_whip.mkv` |
| `--host` / `--client-address` | `198.19.0.10` / `198.19.0.11`，必须是当前隔离环境已有可达地址 |
| `--rtmp-port` / `--rtsp-port` / `--http-port` | `11935` / `18554` / `18080` |
| `--gb-port-base` | `22000`，GB TCP passive 使用的端口区间须预先确认空闲 |
| `--workers` / `--server-cpus` / `--client-cpus` | `8` / `0-7` / `8-15` |
| `--duration` / `--warmup` | `30` / `5` 秒 |
| `--ready-timeout` / `--progress-interval` | `60` / `5` 秒；HLS 固定使用 6 秒进展区间，duration 至少 6 秒 |
| `--ramp-per-second` / `--group-size` | `50` / `256`；group-size 是每个客户端进程的最大 viewer 数，不是服务端并发限制 |
| `--min-free-memory-mib` / `--min-free-disk-mib` / `--max-server-pss-mib` | 均 `0`，表示不限制；非零预算在启动、ready、warmup 和稳态阶段检查，准备期每秒检查；Pi 正式执行须明确设置安全预算 |
| `--fault` | `none`；支持 `invalid_token, expired_token, replayed_token, source_stop`，不是容量输入 |

输入为拉流或 GB 时，还启动上游实例，监听协议端口为相应主端口加 100，绑定 client-address。应一并核对这些端口。UDP 媒体端口池仍遵循现有 P acquire/release、bind 冲突直接失败，不由本工具修改。

## 最小功能运行

在解包目录运行；先核对地址、端口、CPU affinity 和依赖，不直接复制到 Pi 正在使用的 namespace。下例为 DEV loopback 功能验证，不是 TEST 性能结果：

```bash
python3 bench/perf_run.py \
  --output /var/tmp/tool-smoke-rtmp-flv-001 \
  --input rtmp --outputs flv:1 --sources 1 \
  --host 127.0.0.10 --client-address 127.0.0.11 \
  --rtmp-port 45935 --rtsp-port 45954 --http-port 45980 \
  --server-cpus 0-3 --client-cpus 4-7 --workers 4 \
  --warmup 1 --duration 6 --progress-interval 2
```

前提是当前允许的 affinity 包含这些 CPU，且端口未被其他服务占用。可将输出改成 `rtmp:1,rtsp:1,flv:1,hls:1,whep:1` 做少量混合功能验证。WHIP 输入的音频路径与 AAC 标准 fixture 不同，不应和 RTMP AAC→Opus 的成本直接比较。

existing 每个 viewer 都调用 `/api/play` 取得独立凭据；返回 stream_id 必须与 sources-file 相同，变化即停止测量，不隐式改换源。

existing 模式的 `--server-bin` 必须指向当前 DUT 实际可执行文件，而不是任意占位路径。准备探针之前比对 `/proc/PID/exe`，固定 starttick、exe 和 network namespace；采样继续核对同一身份。错误 PID、PID 复用、exec 或 namespace 改变记 `BLOCKED/PROCESS_IDENTITY`，不作为服务器容量失败。外部 DUT 消失记 `INCONCLUSIVE/UNCONFIRMED`；外部发布者消失记 `UPSTREAM_FAILED`。自管进程先保存启动 tick，媒体 ready 后再固定 exec 完成后的完整身份。

同一 ELF 的多个实例还要核对 URL/监听 socket 的 PID/FD 归属。DUT、发布者、上游、signaling、runner 和客户端成本分别归因；客户端先饱和不能作为 DUT 容量上限。

## ready、窗口与 PASS

1. 发布源通过 FLV 双轨媒体探针；仅 registry 存在不算 source ready。
2. 所有 viewer 进入 `media_ready`，之后写 `measurement.start`，内容为本机 CLOCK_MONOTONIC 纳秒。
3. warmup 后各客户端依据共同起点测量；准备、授权、连接开销不放入稳态窗口。
4. `groups[].measurement` 保存客户端原始计数。成功要求全部 requested viewer ready、持续 progressing、客户端正常退出。
5. `groups[].window` 保存真实起止、实际时长、与请求起止的偏差及重叠覆盖。固定要求覆盖请求窗口至少 99%；这用于容纳定时器调度误差，不意味着可用 99% viewer 替代全部 viewer。缺失起止或显著缩短窗口失败，不能按请求时长伪造实际数据。
6. 服务端异常退出、观测到 queue-full/发送缓冲错误/UDP drops、上游异常退出或清理失败，都不能 PASS。客户端 CPU/FD/UDP 资源压力会使容量判定不确定或 BLOCKED。

H.264/AAC 消息、RTP 双轨或独立 HLS 分片进展不等于真实音视频解码；浏览器/FFmpeg 解码需独立验证。

## 长期运行边界

资源 samples 在内存中累计，运行结束才写入完整 JSON；没有增量落盘或按时长限制采样内存。自管 WHIP publisher 固定运行 86400 秒，准备与 warmup 也占用其寿命，因此 24 小时 measurement 会跨过发布者结束点。长时任务先验证采样成本、发布者持续性、结果落盘和清理，不能只增大 `--duration` 就声明支持 soak。

## 输出与清理

新目录保存 `result.json`、server/publisher/client/probe 日志、凭据文件、共同 barrier，以及协议子客户端结果。资源采样包含实际单调时刻、CPU、RSS/PSS、FD、线程、UDP/TCP 队列和 namespace 网络计数。

非零预算的 `budget_samples` 保存准备、warmup、measurement 的实际检查时刻、可用 RAM/磁盘和 DUT PSS。超限立即停止本轮，不继续创建 viewer。UDP drop 端点聚合负差可能源于 socket 关闭，原负差保留并记不确定，不会被当作 `SERVER_UNSTABLE`，也不会静默归零。

SIGTERM/SIGINT 请求有界清理并保存失败结果。仅清理本轮 Popen 对象、自管 receiver/sender；existing 的目标 PID 仅采样。清理结果必须检查 `all_owned_processes_reaped`、`remaining_owned_receivers`、`errors`，不能只看主程序退出。

`--fault source_stop` 是异常源中断注入：测量中点对本轮仍活跃的 publisher 执行 `kill()` 并有界等待（5秒），记录请求及实际完成单调时间。它不使用正常清理的10秒SIGTERM宽限，否则短窗口内可能未真正断流。实际完成在测量结束之后不能算有效负对照；正常结束的清理策略保持不变。

凭据文件、source ID、原始日志可能敏感；目录为 0700，结果不得未经脱敏公开。配置/命令脱敏不保证原始协议日志完全脱敏。源码 HEAD 与运行包的 binary SHA/build manifest 必须一起归档，不能拿工具 HEAD 代替实际运行二进制版本。

运行状态与证据等级按 [结果格式](result_format.md) 解释；实际媒体验收以该次运行的原始结果为准。需要故障对照时使用 [perf_validate](perf_validate.md)，正式执行门槛见 [性能测试规范](../performance_testing.md)。
