# perf_publish.py

## 用途与依赖

通过真实 Go signaling 的 push-device API 签发发布 token，启动固定素材发布；`--cycles` 停止并重新签发同一逻辑设备的独立 generation。不测播放容量，不自动重连，不是 SIP 设备模拟器。

需要已运行的 media_server/signaling，独立测试数据库、正确 media 地址/端口、HTTP 控制 Bearer 配置。RTMP/RTSP 用 FFmpeg；WHIP 用 native `whip_publisher`。所有输入均用 `http_flv_fanout` 验证真实双轨首媒体。普通 fixture 为 H264/AAC，WHIP fixture 为 H264/Opus；文件 SHA 写入结果。

## 参数

`--output`（不存在的新目录）、`--signaling-url` 必填。`--input` 为 rtmp（默认）、rtsp_tcp、rtsp_udp、whip。`--sources 1`、`--cycles 1`、`--duration 60`（每个 ready generation 保持秒数）、`--ready-timeout 60`、`--ramp-per-second 50`、`--group-size 256`。`--client-address 198.19.0.11`、`--client-cpus 8-15`。默认 `--client-dir build`、`--ffmpeg-bin bin/ffmpeg`、`--fixture .cache/perf_fixture_720p30.flv`、`--whip-fixture .cache/perf_fixture_whip.mkv`。正数校验，不支持追加至旧输出目录。

## 示例

先启动已配置的真实 signaling/media，再执行：

```bash
python3 bench/perf_publish.py --help
ROOT=$(mktemp -d /var/tmp/media-publish.XXXXXX)
python3 bench/perf_publish.py --output "$ROOT/publish" \
  --signaling-url http://127.0.0.1:9090 --input rtsp_udp --sources 1 \
  --fixture /absolute/fixture.flv --ffmpeg-bin /home/gyl/bin/ffmpeg \
  --client-dir build-tooling --duration 30 --cycles 2
```

运行包内 client-dir 使用 build，ffmpeg-bin 使用 bin/ffmpeg，fixture 使用 `.cache/perf_fixture_720p30.flv`（WHIP 使用 `.cache/perf_fixture_whip.mkv`）。DEV 隔离例子显式用 `--client-cpus 4-7`，existing 播放同时指定不重叠的 `--server-cpus 0-3`。

`phase=media_ready generation=N` 后，`sources.json` 才原子更新；用 perf_run existing 读取它：

```bash
python3 bench/perf_run.py --output "$ROOT/viewers" --input existing \
  --sources-file "$ROOT/publish/sources.json" --signaling-url http://127.0.0.1:9090 \
  --server-pid "$DUT_PID" --control-token "$CONTROL_TOKEN" --server-bin build-tooling/media_server \
  --client-dir build-tooling --outputs flv:3,whep:3 --duration 10
```

该命令需要真实且当前的 DUT_PID/CONTROL_TOKEN；existing 不启动或关闭外部服务。cycles 更新后，旧 sources.json 快照对应旧 generation，不能悄悄换成新流。播放者退出后不会自动停止发布者；由 publisher 周期或显式停止控制。

## 输出、成功与失败

`sources.json` 含 stream_id、play_body、publisher_pid、publisher_starttick；generation 的唯一 stream_id 是发布 token，属于敏感数据。existing 播放以 PID/starttick 固定发布者身份，不能使用缺失 starttick 的旧快照。另有每个发布/双轨探针日志、路径凭据、`result.json`。记录实际开始/结束单调时间、fixture hash、ready generations、errors/cleanup_errors。

退出 0/PASS：配置/素材成功，要求的 generation 完成 readiness 并保持发布进程存活，owned publishers 回收且设备删除无错误。SIGTERM/SIGINT 在至少一个 generation ready 后视为正常操作停止，结果标 `stopped_by_operator`；这不证明完成所有 cycles，也不证明保持期间持续双轨增长。连续媒体验收由 perf_run 客户端完成。

退出 1/FAIL：缺素材、affinity 错、签发/发布/双轨 readiness 失败、发布进程提前退出或清理错误。setup 失败也保存 result，不以 traceback 代替证据。自己创建的设备会 stop/DELETE、自己创建的进程会停止；不停止外部 DUT/signaling。

本工具创建真实信令持久记录和发布会话，需独立数据库。结果目录 0700；不得公开原始 URL/token。源替换与少量 viewer churn 是功能回归，不能把 cycles 运行写成长期 soak 或容量边界。

WHIP 子进程的固定 duration 为86400秒，包含 generation 准备时间；外层 `--duration` 不会延长它。超过该寿命的保持任务不能视为受支持的长期发布。
