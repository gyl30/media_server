# 协议客户端

这些客户端是现有 `perf_run.py` 的负载发生器，不是另一套部署框架。正式测试优先使用 [perf_run](perf_run.md)；直接客户端只接收已经签发的独立凭据，不关闭认证、不替操作者签发 Token。

## 构建、输入和共同边界

- 在 DEV 用 `-DMEDIA_SERVER_BUILD_BENCHMARKS=ON` 构建（实际 CMake 开关见 [environment](environment.md)）；native 客户端链接项目 Boost/OpenSSL/libSRTP，WHIP 另链接 FFmpeg libavformat。
- 以下示例从独立运行包根目录执行，`build/` 为客户端，`bench/` 为 Python 脚本。需有实际存活的 media_server/signaling 和合适 fixture。
- `play.paths`、`whep.urls`、`whip.urls`、`hls.urls` 由正式 API 签发，一行一个请求凭据，不加引号；行数须与 viewer/source 数一致，不允许空项或重复凭据。文件权限建议 `0600`。不要将例子中的文件名当作工具自动创建文件的能力。
- 每个播放请求使用独立一次性 Token。多 viewer 不能重复同一个 URL。认领后失败也不恢复 Token，重跑必须重新签发。
- native 只证明协议媒体包/消息持续进展，不证明 H264 解码、Chrome 画面或扬声器声音。需要解码证据时使用独立 FFmpeg/Chrome 验证，不把负载接收统计当成解码统计。
- 测量前默认要求视频和音频都有实际媒体；仅视频 fixture 必须显式 `--video-only`（WHIP 不支持此选项）。
- 单调时钟 `--start-file` 文件内容为同一主机的 CLOCK_MONOTONIC 纳秒时间，正式 runner 负责 barrier。不能在两台独立主机间直接共享其值。
- CLI 不创建 namespace，不修改 sysctl，不提升全局 FD 上限。独立客户端的 stdout/stderr 保存到日志；native 不自动生成 `result.json`，统一 runner 才生成正式结果。

以下命令说明当前接口及运行前提，不代表已运行。每次任务按 [结果格式的证据等级](result_format.md#证据等级) 记录实际覆盖，不从历史验收继承 PASS。

工具默认素材是 H264/AAC，WHIP 用 H264/Opus；不是所有生产 codec 的通用生成器。当前 GB PS 输出拒绝 Opus，HLS 不封装 Opus，native WebRTC 的 offer 范围见下文。协议可连接不证明该素材在所有输入×输出组合中都兼容，未实测组合单独标记。

## RTMP：`build/rtmp_fanout`

用途：消费真实 RTMP 播放授权路径。输入 `stream_id/play_token`，由 RTMP app 与 stream 分别携带两部分。仅播放，发布走 [perf_publish](perf_publish.md) 的 FFmpeg。

```bash
build/rtmp_fanout --help
build/rtmp_fanout --play-paths-file play.paths --viewers 1 \
  --media-host 198.19.0.10 --media-port 11935 \
  --warmup 1 --duration 10 --progress-interval 2 >rtmp.log 2>rtmp-error.log
```

关键默认：host `127.0.0.1`，port `1935`，warmup `5s`，duration `10s`，ready timeout `60s`，progress `5s`，ramp `50/s`。`--viewers` 必填；`--play-paths-file` 与 `--stream-name PATH` 二选一。后者只适合一个已经签发的路径/故障探针，多 viewer 要使用独立凭据文件。

首媒体：收到真实非 sequence-header FLV 视频包，默认还需真实音频包。持续进展按视频/音频消息计数，视频/音频时间戳不能倒退。首媒体包含网络连接/授权/等自然关键帧时间，不是纯服务器处理延迟。成功需全部 viewer ready、测量窗口持续进展，退出 `0`；媒体/网络失败 `1`；参数/凭据错误 `2`。输出 `phase=media_ready/measurement_start/measurement`、第一失败原因、计数及首媒体分位数。取消 socket/resolver，进程退出释放连接；不创建/删除 publisher。

## RTSP：`build/rtsp_fanout`

用途：DESCRIBE→SETUP→PLAY，**只支持 TCP interleaved 播放**。RTSP UDP publish 的支持不能推导成 UDP play 或 UDP pull。

```bash
build/rtsp_fanout --help
build/rtsp_fanout --play-paths-file play.paths --viewers 1 \
  --media-host 198.19.0.10 --media-port 18554 \
  --warmup 1 --duration 10 --progress-interval 2 >rtsp.log 2>rtsp-error.log
```

参数与 RTMP 共用，默认端口 `8554`。路径按已签发 URL 的 path 去掉前导 `/` 保存，不能用未授权逻辑名称替代。

首媒体不是 PLAY 200：必须收到 SDP 对应视频 RTP，默认还需音频 RTP。按 SDP track 顺序分轨；RTP version、sequence 连续性和时间戳检查。计数为 RTP 包，不是完整 AU/已解码帧。成功、失败、超时、输出与清理同 RTMP；协议库在运行中排队的控制写入也随 client 销毁释放。无法把此工具当 UDP 播放测试。

## HTTP-FLV：`build/http_flv_fanout`

用途：真实 HTTP 200/chunked FLV 解析。凭据文件路径为 `stream_id/play_token.flv`，没有 query Token。

```bash
build/http_flv_fanout --help
build/http_flv_fanout --play-paths-file play.paths --viewers 1 \
  --media-host 198.19.0.10 --media-port 18080 \
  --warmup 1 --duration 10 --progress-interval 2 >flv.log 2>flv-error.log
```

默认端口 `8080`，其他共用参数同 RTMP。HTTP 200 或 FLV header 不算 ready：必须实际解析 H264/H265 media 回调，默认还需实际音频 media 回调；检验 tag size 和轨道时间戳。字节是解析后的媒体 payload，并非所有 HTTP wire bytes。失败/退出码与共用 fanout 一致，403 可识别 authorization。按 streaming body 有界读取；客户端不创建服务端额外控制资源。

## WHEP：`build/whep_fanout`

用途：native ICE/DTLS/SRTP 接收，检测逐 viewer 视频/音频真实进展与 RTP 健康。当前生成的 offer 使用 **H264 PT102 / profile-level-id=42e01f**及 **Opus PT111**；服务端或Chrome能播放H265/PCMA/PCMU不等于此native client提供这些能力。

```bash
build/whep_fanout --help
build/whep_fanout --whep-urls-file whep.urls --sources 1 --viewers 1 \
  --bind-address 198.19.0.11 --io-threads 2 --ramp-per-second 10 \
  --warmup 1 --duration 10 --progress-interval 2 >whep.log 2>whep-error.log
```

关键默认：sources `1`，io threads `4`（范围1..64），ramp `100/s`，bind `0.0.0.0`，warmup `5s`，duration `15s`，ready timeout `60s`，progress `5s`。URL 与 `--whep-url` 二选一；正式多 viewer 必须用独立 URL 文件。`--sources` 是配置标签/源数量，不自动签发或创建 source。

ICE/DTLS 成功不算 media ready；SRTP unprotect 后视频/默认 Opus 包均到达才 ready，检查 RTP sequence 和 timestamp。每个进展窗口均需所有 viewer 双轨增长；`--video-only` 取消音频要求。测量完成逐 session 关闭 peer 并对 Location 发 DELETE，检查 `stopped=removed=requested`。失败启动已有 HTTP 201 的资源也尝试 DELETE。

输出 `established`、`media_ready`、`progress`（含完整/尾窗口和实际时长）、`measurement`、`disconnect`。`received_bytes` 包括解密后 RTP/RTCP，并非编码 elementary bitrate、网卡 wire bitrate 或解码率。退出 `0/1/2` 分别为完整成功、媒体/清理失败、参数/内部异常。所有测量与 DELETE 证据必须保留，不能只看 HTTP 201。

同机测试 server `127.0.0.1` 时 client 用 `--bind-address 127.0.0.2`，或使用已批准 namespace 地址隔离；不改变 server bind-failure 不重试语义。

## WHIP：`build/whip_publisher`

用途：从已准备的 **H264 + Opus MKV** 共享 fixture 发真实 WHIP SRTP，逐 source 独立 publish Token。

```bash
build/whip_publisher --help
build/whip_publisher --whip-urls-file whip.urls --fixture .cache/perf_fixture_whip.mkv \
  --sources 1 --bind-address 198.19.0.11 --io-threads 2 \
  --ramp-per-second 10 --duration 10 >whip.log 2>whip-error.log
```

默认 duration `15s`，ramp `100/s`，io threads `4`，bind ANY；fixture、URL 文件、sources 必填。不接受旧 `--whip-base-url`、`--stream-prefix`、`--bitrate`。不存在 `--video-only`、共同 `--start-file` 或独立 warmup；正式 runner 在 publisher ready 后通过接收探针建立自己的稳态窗口。

fixture 必须真实可解析，有 H264/Opus 双轨。编码和拆包在共享 fixture准备阶段完成，不是每个 source 启动 FFmpeg。发送计数仅证明客户端发出包；服务端实际收媒体由正式 runner 的授权 FLV双轨探针确认，发布者不自称浏览器解码成功。

输出 established/measurement/disconnect；保留 RTP 发包、运行错误、Location DELETE 与 removed。支持 SIGTERM/SIGINT 有界停止并 DELETE 已创建资源。参数异常 `2`，运行/清理失败 `1`；正常完成 `0`。WHIP输入Opus→非WebRTC输出时的codec转换范围以服务端当前能力为准，不把任意RTSP/RTMP音频组合声称为支持。

## HLS：`bench/hls_client.py`

```bash
python3 bench/hls_client.py --help
python3 bench/hls_client.py --hls-urls-file hls.urls --viewers 1 \
  --warmup 1 --duration 12 --progress-interval 6 --output hls-result.json >hls.log 2>&1
```

仅 Python标准库；输入 HTTP HLS 授权入口 URL，预期307跳转 session path。默认warmup `5s`、ramp `50/s`、ready `60s`、progress `6s`；duration/viewers/URL文件必填，duration必须覆盖progress interval。`--persistent` 复用HTTP连接，默认每请求新连接，两种配置不要混合比较。只实现 `http`，HTTPS UNSUPPORTED。

新 viewer 从尾分片开始；同sequence只下载一次，检测retention gap/ENDLIST。完整drain新媒体分片后才ready；每进展窗口每viewer均需新增unique segment。**不解析TS/fMP4独立音视频轨，不验证解码/音频采样增长。** 同一分片重复下载不增加segments。

输出 optional JSON (`status/ready/requested/viewers/errors/measurement_*_monotonic_ns/client_cpu_seconds`) 和phase日志，PASS/0需全员测量且unique segments增长；失败/1。HTTP连接5s，header/body10s，整体运行有界；取消asyncio tasks并关闭writers。HLSsession结束后服务端retention由现有生命周期控制，客户端退出不等于分片文件立即释放。

## GB28181 输出：`bench/gb_sender_fanout.py`

这是**media_server sender → 工具 RTP/PS sink**，不是模拟设备注册、不是向 server receiver 发布。现有源须由 signaling合法创建，control Token是独立媒体管理Bearer凭据，不是play Token。

```bash
python3 bench/gb_sender_fanout.py --help
python3 bench/gb_sender_fanout.py --control-url http://198.19.0.10:18080 \
  --control-token "$PERF_CONTROL_TOKEN" --streams-file source-streams.txt \
  --host 198.19.0.11 --transport udp --viewers 1 --warmup 1 --duration 10 \
  --progress-interval 2 --output "$PERF_NEW_OUTPUT"
```

以上变量由操作者从本轮私有测试凭据/新输出目录设置，不能复制真实密码到公开命令记录。streams file 一行一个已存在stream_id，多个sender按源列表轮转；亦可互斥 `--stream-name ID`。无需每viewer播放Token，因为此处是控制授权创建sender。

默认host `127.0.0.1`，PT96，ramp100/s，warmup3s，duration15s，ready60s，progress5s，listen-port0。必须 duration≥progress。`--transport udp|tcp_active|tcp_passive`：

| 模式（server角色） | 工具角色 | 端口要求 |
|---|---|---|
| udp | 每sender本地UDP接收socket | 临时端口，地址隔离 |
| tcp_active | 一处TCP listener接受server连接 | `--listen-port 0`可分配临时端口 |
| tcp_passive | 主动连接每个server listener | 非零`--listen-port`起点及连续viewer数量的可用server端口 |

输入RTP检查SSRC/PT/sequence并重组PS/PES。实际视频PES、默认实际音频PES到达才ready；检查PES PTS不倒退；测量按各SSRC双轨进展，非“字节>0”即成功。未执行FFmpeg解码，不能宣称厂商互通或画面正确。

创建sender、socket/listener；结束/失败DELETE所有attempted sender，关闭tcp tasks/writers/UDP transports。JSON包含establishment/measurement/cleanup/errors，PASS/0要求全员进展、无RTPseq错误、DELETE完整、无清理错误；FAIL/1。创建响应不明确也保留删除尝试，404清理失败要如实记录，不吞掉。

GB28181 输入由 `perf_run.py --input gb_udp|gb_tcp_active|gb_tcp_passive`以独立upstream media_server sender驱动DUT receiver，参见perf_run文档；不是SIP simulator E2E，当前MVP SIP设备点播不因此获得TCP业务入口支持。

## 内部 microbench：`build/gb_ps_fanout` 与 `bench/gb_ps_fanout.py`

用途为共享PS准备、packetizer和callback成本，不连接media_server外部服务，不使用授权，不产生网卡吞吐或解码验收。

```bash
python3 bench/gb_ps_fanout.py --help
python3 bench/gb_ps_fanout.py --client-bin build/gb_ps_fanout --viewers 1 \
  --duration 2 --frame-bytes 16000 --output "$PERF_NEW_OUTPUT"
# native直接入口（位置参数，不支持--help）
build/gb_ps_fanout 1 2 16000
```

native默认100viewer/10s/16000bytes；wrapper默认duration20/frame16000，viewers/output必填。固定6workers，人工H264填充AU + 合法PCMA静音，RTPcallback检查SSRC、sequence、音视频最低计数。视频填充不是合法fixture，**不得用于真实视频解码结论**。stdout JSON/phase，wrapper补进程资源/result.json；资源限于自有worker/source/senders，由现有shutdown/request_stop回收。无独立warmup/barrier，不能与正式TCP/UDP/WHEP容量结果直接比较。

## 故障辅助：`build/webrtc_disconnect`

此 target 不在当前 `package_runtime.py` 固定 ELF 清单中。以下命令仅在 DEV 已构建该 target 的目录执行；TEST 专项验证必须另行复制并核对 ELF/依赖/SHA，不能假定默认运行包包含它。

```bash
# 必须从已创建会话保存真实answer；STDIN必须文件/pipe，不能继承交互终端等待
build/webrtc_disconnect play established < answer.sdp >disconnect.log 2>&1
```

固定两个位置参数：direction `publish`或其他（作play）、expected `established`或其他（期望DTLS timeout）；没有`--help`。输出offer后从STDIN读answer至EOF，真实建立peer并关闭，验证结果与期望一致。只关闭自己的peer，不DELETE外部POST资源，操作者必须用原Location关闭对应server session。错误参数数量`2`，断言不匹配`1`，匹配`0`。只作定向协议故障探针，不进行媒体进展，不是正式性能工具。

## 已被替代的 RTSP 合成发布器

`build/rtsp_publisher`保留历史micro-generator实现，没有正式独立凭据输入，视频为synthetic filler而非可解码fixture。当前授权产品下不作为正式输入；真实素材和授权发布使用 [perf_publish](perf_publish.md) 或 [perf_run](perf_run.md)。不要将它成功发送字节写成真实音视频发布验收。
