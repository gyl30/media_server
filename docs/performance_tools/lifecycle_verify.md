# 生命周期、源替换和 viewer churn

## `bench/lifecycle_verify.py` 的用途与当前状态

旧工具通过真实协议client/FFmpeg/browser辅助覆盖public endpoints，保存source replacement、consumer wave、异常断连和资源趋势。它不是perf_capacity驱动，也不提供正式性能最大值。

当前共享`Run`仍使用旧`live/perf0`无TokenURL、缺少对应signaling/control凭据、自造旧WHIP参数，并向HLS client传已删除`--hls-url`。**CODE REVIEWED；DEV --help VERIFIED；媒体suite BLOCKED / NOT_COMPATIBLE_CURRENT_AUTH。** 此事实也影响引用Run的codec/browser/UDP RTCP验证脚本。

本轮不重新实现第二套生命周期框架、不删除已有有价值断言、不用关闭授权恢复旧工具。优先用现有`perf_publish --cycles`、合法`perf_run`、`perf_validate`以及CTest完成当前可执行回归，缺的同逻辑源跨generation消费者证明如实单列。

## 依赖与参数

依赖build目录的media_server、rtmp/rtsp/whep native clients、WHIP publisher；真实FFmpeg fixture/CLI、Python标准库，部分场景调用gb_network/http_flv_fanout辅助；断连场景还可能编译本地socket故障注入DSO，源码运行环境不能与纯bundle混淆。

```bash
python3 bench/lifecycle_verify.py --help
```

| 参数 | 真实含义/默认 |
|---|---|
| 位置suite | smoke / replacement / churn / soak / disconnect |
| `--build-dir` | 必需；server/native目录 |
| `--client-dir` | 默认build-dir；可单独指定native目录 |
| `--fixture`、`--output` | 必需；fixture可解码，output应为本轮新目录 |
| `--ffmpeg` | /home/gyl/bin/ffmpeg |
| `--port-base` | 21930；RTMP/RTSP/HTTP及其他端口相邻使用 |
| `--generations` | 20，replacement计数 |
| `--inputs` | 默认rtmp rtsp whip gb；replacement选择输入 |
| `--gb-transport` | udp（默认）/tcp_sender_active/tcp_sender_passive |
| `--rtsp-publish-transport` | tcp（默认）/udp，发布而非UDP播放 |
| `--iterations`、`--batch` | 300、20；iterations须被batch整除 |
| `--duration` | 600s，soak截止，不是各客户端统一正式测量窗口 |

只读最小示例就是上面的`--help`，无网络副作用。当前不提供会误导操作者的“正常可运行suite”命令。静态参数检查可用`python3 bench/lifecycle_verify.py churn --build-dir build --fixture .cache/perf_fixture_720p30.flv --output "$PERF_NEW_OUTPUT" --iterations 3 --batch 2`，预期argparse非零且**不会创建server**；该例是故意非法输入，不是负载成功。

## 每个suite的独立职责

### `smoke`

旧设计逐RTMP/RTSP/WHIP publisher，单/多consumer wave，另以FFmpeg解码输出。RTMP/RTSP默认要求视频+音频实际计数，WHEP要求解密后两轨及DELETE；HTTP旧helper主要字节检查不能替代native完整FLV解析。旧权限入口BLOCKED，不得用历史结果作为当前PASS。

当前可执行替代入口（在获准独立环境，文件存在，output新建）：

```bash
python3 bench/perf_run.py --output "$PERF_NEW_OUTPUT" --input rtsp_udp \
  --outputs rtsp:1,flv:1 --warmup 1 --duration 10 --progress-interval 2
```

该命令只证明正式runner这组小媒体功能，不能声称跑过旧suite全矩阵/真实FFmpeg解码。更多协议例子见[协议client](protocol_clients.md)。

自行管理输入的perf_run使用测试SignalingStub；真实Go signaling跨请求授权应另外通过existing模式及真实play_body验证，不将两者混写。

### `replacement`

旧设计同逻辑publisher反复停止/重建，检查旧consumer结束、新consumer可用、旧HLS playlist仍属于旧generation且新Location不重复。不是简单`source_stop`；新source对象和新一次性凭据都必须可追溯。

复用已有正式publisher，不创建新framework：

```bash
python3 bench/perf_publish.py --help
python3 bench/perf_publish.py --output "$PERF_PUBLISH_OUTPUT" \
  --signaling-url http://198.19.0.10:9090 --input rtmp --sources 1 \
  --fixture .cache/perf_fixture_720p30.flv --ffmpeg-bin bin/ffmpeg \
  --client-dir build --client-address 198.19.0.11 --client-cpus 8-15 \
  --duration 10 --cycles 2
```

这需要已有独立正式server/signaling，不自己创建这些服务。每轮为同一个push_device重新签发publish Token，独立source identity，`sources.json`在ready后替换，`result.json.generations`保存每代身份/ready/owned PID。该命令只能独立证明publish generations，不自动证明旧viewer退出与新viewer增长。

消费者验证必须在当前generation窗口中使用新凭据，不能把旧`sources.json`/ticket反复复用。另一个进程使用：

```bash
python3 bench/perf_run.py --output "$PERF_CONSUMER_OUTPUT" --input existing \
  --sources-file "$PERF_PUBLISH_OUTPUT/sources.json" \
  --signaling-url http://198.19.0.10:9090 --control-token "$PERF_CONTROL_TOKEN" \
  --server-pid "$PERF_SERVER_PID" --server-log "$PERF_SERVER_LOG" \
  --outputs flv:1,whep:1 --warmup 0 --duration 5 --progress-interval 1
```

这两个进程需要人工/已有测试编排对齐；上述示例DOCUMENTED / REQUIRES_TEST，不宣称已实现自动跨代接力或consumer自动重连。publisher的10秒窗口只是小示例，不应机械用于正式性能。新consumer窗口不足/换代途中授权失败应保留为单独生命周期证据，不能进入稳态容量F边界。

### `churn`

旧设计批量consumer waves，以及WHIP/GB创建销毁、HLS session计数，默认300×batch20对本轮过大，且旧入口BLOCKED。

当前复用同一个健康source，以`perf_run --input existing`多次新output，runner每次独立签发全部viewer URL，关闭后核对DELETE/session/自有进程。每一轮必须重新取源身份且记录generation；不能复用已经消费的token文件。小规模2或3轮、每轮1到3viewer足够做本轮功能验证；**不能把这种短回归写成长期稳定性或正式容量**。本轮实际执行与未执行项以交接结果为准，不因文档存在自动PASS。

### `soak`

旧设计time-bounded混合输入、consumer waves、source generations及resource samples。当前BLOCKED，且本Goal明确不执行长soak。待Pi未来批准后另选正式负载、30分钟等真实窗口。不能用秒级smoke声称soak通过。

### `disconnect`

旧设计连接建立中断、peer reset、DTLS断开等；仍依赖旧启动/授权路径。现在先复用已注册的session lifecycle/transport/DTLS CTest与`perf_validate cancel/source_stop`负对照。CTest mock/in-process证据与真实跨主机网络不能互换；没有执行的故障不得报告PASS。

## 结果、错误、清理和边界

- 历史成功后输出`result.json`包含HEAD/suite/config/result，`commands.json`保留命令，自有服务/clients日志及resources snapshots。失败异常可能早于result写出，不能按“没有FAIL字段”视为成功。
- 原脚本`Run.close()` ExitStack停止自有进程，再检查server退出0、sanitizer文本和write queue full；不管理别人的PID、不全局清理端口。因为当前入口BLOCKED，此清理源码检查不是本轮实际全suite资源验收。
- HTTP-FLV旧字节阈值、nativeRTP包计数、FFmpeg解码、Chrome帧/采样是不同证据等级。源替换应另记录generation身份，不能根据URL文本同名断言对象相同。
- 正式runner的cleanup JSON、各client最后phase、server日志、PID/starttime、端口/FD回落一起复核；HLS retention正常保留窗口与泄漏区分，不强制立即内存归零。
- Token认领成功不可恢复，source停止后再publish必须重新签发；即使logical device相同也不是同一个source generation。
- source_stop是预期故障；工具错误、客户端资源不足、server失稳、窗口不足须分别分类。负对照成功不代表容量失败边界。

## Pi 的验收顺序

1. 先运行参数/offline及对应CTest，确认固定端口锁和独立构建。
2. 新output单source/native1viewer合法smoke，验证两轨真实进展和cleanup。
3. 新output授权负对照，不把预期失败当capacity F。
4. `perf_publish --cycles 2`与独立consumer waves取得源替换/churn证据，或明确NOT_RUN/TEST_GAP；不复制通用生命周期runner。
5. 正式性能/容量仅在工具正确性与环境健康后开始，使用独立目录与负载；不混入上述故障/短周期证据。

参见[Pi交接手册](pi_handoff.md)、[旧入口](legacy_tools.md)、[功能验证](functional_verification.md)。
