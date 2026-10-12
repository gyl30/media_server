# 旧性能入口与替代方式

本页专门记录仍在仓库的旧CLI，避免把 `--help` 能运行误认成正常媒体负载可用。**DEPRECATED**指已由统一工具替代；运行媒体验收在当前授权实现下为 **BLOCKED / NOT_COMPATIBLE_CURRENT_AUTH**，不是生产服务器容量不足。

本轮没有删除这些文件、恢复旧URL或增加兼容wrapper。每个入口都有下面的专属说明。只读参数帮助是安全的最小示例；以下旧入口的完整媒体命令不建议执行，也没有作为本轮PASS。

## 共同问题、输出和资源

旧直接脚本普遍自行启动server但未连接本轮signaling授权，发布/播放使用 `live/perf0` 等无独立Token路径；`fanout_support.wait_for_stream()`也使用旧无Token FLV预检。它们可能在listener打开后等待失败，而不是产生可用于性能结论的媒体窗口。`mixed_fanout`还传已经删除的HLS参数，`whip_publish`还传已经删除的WHIP参数。

- 依赖DEV构建native clients、FFmpeg fixture/CLI和Python；旧路径默认通常是 `build/` 和 `/home/gyl/bin/ffmpeg`，不是可移植bundle保证。
- 旧脚本若到达测量会写 `server.log/publisher.log/client.log/result.json`及资源摘要；早期异常可能只有日志没有完整FAIL JSON。不要把旧result结构当统一 [result_format](result_format.md)。
- 多数 `--output` 使用 `exist_ok=True`，会截断旧同名日志；不得重用旧证据目录。
- `finally`调用`stop_process()`仅停止自己的Popen对象，SIGTERM最多等待10秒再kill/wait；它们不是异常父runner被强杀后的进程组清理保证，也不校验端口完全回收。
- 正式性能数据必须走统一runner，保留manifest、实际窗口、health与cleanup，不能从旧wrapper的字节率补造PASS。

以下替代命令从已解包且依赖完整的运行包执行，TEST须处于**本轮获准的独立namespace**。地址/CPU默认为TEST约定，不得占用Pi活动namespace。输出路径必须新建且不存在；命令是DOCUMENTED / REQUIRES_TEST，不是本轮自动运行正式测量。

```bash
python3 bench/perf_run.py --help
python3 bench/perf_run.py --output "$PERF_NEW_OUTPUT" --input rtmp \
  --outputs rtmp:1 --warmup 1 --duration 10 --progress-interval 2
```

DEV功能验证应显式覆盖host/client-address/CPU/监听端口；见[环境](environment.md)。正式长窗口与容量驱动留给Pi。

注意：`perf_run`自行管理输入时使用测试`SignalingStub`，实际执行媒体授权HTTP边界但不证明Go signaling业务正确；`--input existing`配真实`--signaling-url`及`play_body`才走Go `/api/play`。不能把self-managed stub负载说成完整正式信令E2E。

## `rtmp_fanout.py`

用途：旧RTMP发布→RTMP播放扇出资源测量。参数 `--fixture --output --viewers`必需；server/client默认 `build/media_server`、`build/rtmp_fanout`，FFmpeg `/home/gyl/bin/ffmpeg`，workers6、ramp50/s、warmup5s、duration20s；host127.0.0.1，RTMP11935/RTSP18554/HTTP18080。依赖H264/AAC FLV fixture；旧权限预检使正常负载BLOCKED。输出原client phase及server资源；未启动媒体不算PASS。资源与失败行为见共同说明。

最小帮助：`python3 bench/rtmp_fanout.py --help`。替代：`perf_run.py --input rtmp --outputs rtmp:1`（必须加新`--output`）。

## `rtsp_fanout.py`

用途：旧RTMP输入→RTSP TCP播放。参数/默认与RTMP wrapper基本相同，client `build/rtsp_fanout`；不是UDP播放工具。输出RTSP媒体phase和server资源。当前无Token预检失败，不能用CONNECT或DESCRIBE证明媒体。最小帮助：`python3 bench/rtsp_fanout.py --help`。替代：`perf_run.py --input rtmp --outputs rtsp:1`。

## `http_flv_fanout.py`

用途：旧直接RTMP→Python HTTP-FLV多源/多viewer测量，另提供被旧生命周期脚本调用的viewer辅助。必需fixture/output/viewers，sources默认1、workers6、ramp100/s、warmup5s、duration20s，三监听端口同上。依赖Python、FFmpeg；部分统计按HTTP读取字节而非当前native media counters。默认无播放Token，BLOCKED。输出per-viewer errors和CPU/PSS摘要，不是统一health契约。

最小帮助：`python3 bench/http_flv_fanout.py --help`。替代：`perf_run.py --input rtmp --sources 1 --outputs flv:1`；统一runner使用`build/http_flv_fanout`真正FLV解析client，二者不能混淆。

## `hls_fanout.py`

用途：旧RTMP→HLS下载扇出。必需fixture/output/viewers，workers6、ramp50/s、warmup5s、duration20s，可选`--persistent`。依赖`hls_client.py`，但旧wrapper的`--hls-url`不再是client参数，且URL无Token。BLOCKED。输出server及client摘要/日志，失败在测量前不得做吞吐结论。最小帮助：`python3 bench/hls_fanout.py --help`。替代：`perf_run.py --input rtmp --outputs hls:1 --duration 12 --progress-interval 6`；persistent client的独立用法见[协议客户端](protocol_clients.md)。

## `whep_fanout.py`

用途：旧RTMP多源→WHEP native扇出。必需fixture/output/viewers，sources1、workers6、client-threads8、ramp100/s、warmup5s、duration20s，client-bind-address默认ANY。旧固定WHEP URL没有逐viewer独立Token，BLOCKED；有同IP UDP临时端口冲突风险，不能据其失败认定服务器边界。输出established/measurement/disconnect、队列和资源摘要，旧窗口不能混用新统一数据。

最小帮助：`python3 bench/whep_fanout.py --help`。替代：`perf_run.py --input rtmp --sources 1 --outputs whep:1`；native唯一URL文件使用方式见协议客户端。

## `publish_only.py`

用途：旧多FFmpeg RTMP发布-only。必需fixture/output/sources；workers6、warmup5s、duration20s、host/监听同上。通过旧FLVGET检查source readable，无法在当前授权边界完成ready，BLOCKED。输出publisher_alive/source_readable、publish_first_media_ms及估算published_bytes_per_second；后者不是实际每source已接收吞吐。最小帮助：`python3 bench/publish_only.py --help`。替代：`perf_run.py --input rtmp --sources 1 --outputs none`，统一runner用独立播放凭据探测source实际双轨媒体。

## `rtmp_publish.py`

用途：旧单FFmpeg tee多个RTMP source，避免每源进程。必需fixture/output/sources；默认workers6、warmup3s、duration15s，FFmpeg `bin/ffmpeg`，监听同上。发布URL旧格式，BLOCKED；tee packet发送不证明server实际媒体进展。输出publisher/server资源和活跃source检查，失败清理仅自身publisher/server。

最小帮助：`python3 bench/rtmp_publish.py --help`。替代：统一`perf_run.py --input rtmp --outputs none --sources 1`。新正式工具未承诺单tee进程的相同成本，不把不同generator结构直接横比。

## `rtsp_publish.py`

用途：旧调用synthetic `rtsp_publisher` 做TCP/UDP多源发布。必需output/transport/sources；transport tcp|udp，workers6、client-threads8、ramp50/s、bitrate2000000、warmup3s、duration15s。无真实fixture参数；视频填充流不证明解码。默认无Token，BLOCKED。输出generator sent/ready/CPU与server资源，不可正式验收。

最小帮助：`python3 bench/rtsp_publish.py --help`。替代：`perf_run.py --input rtsp_tcp --outputs none` 或 `--input rtsp_udp`，使用真实FFmpeg fixture。

## `whip_publish.py`

用途：旧合成WHIP多source发送测量。必需output/sources；client-threads8、ramp50/s、bitrate2000000、workers6、warmup3s、duration15s。仍给native传已删除`--whip-base-url/--stream-prefix/--bitrate`，因此直接参数错误，BLOCKED，不是服务器故障。输出旧publisher phases/server资源，不产生可信正常负载。

最小帮助：`python3 bench/whip_publish.py --help`。替代：`perf_run.py --input whip --outputs whep:1`，使用真实H264/Opus MKV与独立WHIP URL文件。

## `mixed_fanout.py`

用途：旧同源RTMP/RTSP/HLS/WHEP各N viewer；不含FLV正式native组。必需fixture/output/viewers-per-protocol，workers6、warmup5s、duration20s。clients硬编码`build/*`，HLS参数已失效，没有逐viewerToken；BLOCKED。客户端窗口duration+10与server窗口也不等长。输出protocol测量与server summary，不能当统一共同窗口结果。

最小帮助：`python3 bench/mixed_fanout.py --help`。替代：`perf_run.py --input rtmp --outputs rtmp:1,rtsp:1,flv:1,hls:1,whep:1 --duration 12 --progress-interval 6`，所有组由正式runner统一barrier。

## `gb_network.py`

用途：旧server GB sender→receiver pair循环，经HTTP-FLV读取做网络资源统计。必需fixture/output/transport/pairs；transport udp|tcp_sender_active|tcp_sender_passive，pairs≤1000，workers6、warmup3s、duration15s、tcp-port-base31000。依赖FFmpeg和旧未认证控制POST；BLOCKED。字节阈值10000B/s不是PS双轨进展验证；DELETE异常吞掉也不提供统一cleanup证据。

最小帮助：`python3 bench/gb_network.py --help`。替代：输入GB用`perf_run.py --input gb_udp --outputs rtsp:1`或对应gb_tcp_active/passive；输出GB用`--input rtmp --outputs gb_udp:1`（TCP输出名称以perf_run当前help/source为准，见其专属文档）。已存在source的独立输出用`gb_sender_fanout.py`。

## `performance_baseline.py`

用途：旧依次重复publish-only、RTSP/RTMP/FLV/HLS 1/4/8及WHEP档位，汇总median。必需build-dir/fixture/output；warmup10s、duration30s、runs3（至少3）、whep-levels默认1 100 250；`--workloads`过滤`protocol-N`，重复/未知拒绝。会调用上述旧入口，BLOCKED；**不要在本轮启动其默认整矩阵**。输出command.json/runner.log/summary.json，子进程异常中止时此前记录不等于全部测试通过。没有正式统一manifest，历史负载结构不能冒充新的TEST基线。

最小帮助：`python3 bench/performance_baseline.py --help`。替代：按照Pi handoff对同一`perf_run`配置使用新的独立output重复；自动容量搜索使用`perf_capacity`，不是此wrapper。本轮不执行正式基线或容量搜索。

## `rtsp_publisher` native

旧独立publisher默认host127.0.0.1、port554、local ANY、transport tcp、sources必填、stream-prefix必填、io4/ramp100/fps25/bitrate710000/warmup3/duration15；支持`--udp-port-start`非零偶数和连续端口对，但只在UDP模式使用。其他参数`--media-host/--media-port/--local-address/--transport/--sources/--ramp-per-second/--io-threads/--fps/--bitrate/--warmup/--duration`。

没有`--help`，无独立Token文件、无真正可解码fixture。当前正式媒体负载DEPRECATED；不提供制造成功的旧无Token执行例子。最小安全参数拒绝检查：`build/rtsp_publisher --unknown-option`（预期2）；不运行网络。正式替代为前述FFmpeg RTSP TCP/UDP输入。

## 验证状态与交接

本轮实际核对上述Python `--help`均退出0；这属于DEV参数帮助核验。当前不兼容依据来自真实调用链、已删除参数和当前授权API，未对每个旧入口重复启动服务制造相同失败。旧路径错误不是正式capacity F边界。Pi应只选正式文档入口，不调用这些旧wrapper建立新性能基线。

功能回归旧入口另外见 [functional_verification](functional_verification.md) 和 [lifecycle_verify](lifecycle_verify.md)，不要因此误认为所有旧代码都可直接删除。
