# 阶段六：Web 麦克风与可信 HTTPS/WSS

本增量接续 `931d285` 的[Qt 采集](qt_monitor_client_stage6_qt_talk.md)，不替代[Go/SIP 对讲 owner](qt_monitor_client_stage6_talk_control.md)或 C++ 音频发送器。Web 只在父国标设备详情中通过用户点击启动麦克风；页面明确显示“正在上送麦克风，设备听音未确认”。观看画面的回传音频仍独立开启，不做独立广播 UI。

## 实现与所有权

- 单个 `TalkCapture` 拥有实际采集、AudioContext、Worklet、WebSocket、在途 POST 与精确会话 ID。没有另建 token 状态、重试、后台线程或关闭标志。
- 必须是安全上下文，实际 AudioContext 必须为 8000Hz。申请单声道麦克风；原生浏览器采样率转换交给 Web Audio，未提供 8kHz 就明确拒绝，不静默丢音频。
- AudioWorklet 每 160 样本产生一个 160 字节 PCMA 消息。量化与项目 FFmpeg S16→PCMA 全值域逐字节一致；单消息 credit 有界，主线程迟到超过 200ms 的消息丢弃，不突发补发积压音频。processor 输出零，不将麦克风回放到本地扬声器。
- 上传握手完成才连接采集图。检查 WS readyState、实际发送进展及 AudioContext 状态。发前 `bufferedAmount > 1660` 终止；这不是总队列严格不超过 1660 字节，也不是内核/端到端延迟保证。
- 准备上限 20 秒、POST 15 秒、WS 握手 5 秒、发送进展 3 秒、DELETE 5 秒。停止立即关闭上传和采集图、结束 track，然后等待已发出的 POST 有界完成；迟到 201 按自己的 ID 删除。重复 stop 共用真实 cleanup promise，不递归触发 UI 回调。
- 路由卸载、标签页隐藏、pagehide、设备离线都请求停止。未返回的权限请求无法由页面强行关闭，但迟到的 track 会停止且不申请对讲。仅有模拟权限拒绝证据，不冒充实际用户操作权限对话框。
- Qt/Web 音频都直接到 C++ 媒体；Go 不转发音频。本轮 TLS 反向代理仅是测试网关夹具，不是新增生产 Go 音频中继。

## HTTPS 部署

新增一个配置 `--media-http-origin https://video.example.test`，是**媒体 HTTP 公开 origin**，不是服务监听或 TLS 终止开关。仅接受 http/https、合法端口、无凭据/查询/fragment、空路径或 `/`，不支持子路径部署。为空时完整保留原 `http://media-ip:media-http-port` 行为。

FLV、HLS、WHEP、WHIP、设备录像 PS 共用该 origin；对讲只派生对应 ws/wss。这样不会出现安全页面上上传已 WSS、回传 WHEP 却仍是 HTTP 的混合内容错误。一个复用 helper 服务多个实际调用点，不再新增单独 audio-upload-origin 配置。

不信任请求 Host/Forwarded 生成公开地址。不改变 `--media-control-url`、SIP SDP 的 `--media-ip`、RTMP/RTSP、一次性认领或主动 WHEP 拉流的既有支持范围。

网关必须转发媒体完整根路径，包括 WHEP/WHIP session 的 GET/DELETE、HLS session/TS 和音频 WebSocket Upgrade，而不只是首个 POST。证书必须由浏览器正常信任；不忽略证书错误。WebRTC UDP/ICE 可达性仍需按原配置保证，公开 HTTPS origin 不解决 NAT。

## 实际验证

DEV 证据根：`/var/tmp/qt-web-talk-931d285-RdEN2jof/`。测试使用独立端口、数据库、自己的 Chrome profile、测试 WAV 和本轮短期 CA。CA 只进入本轮私有 NSS 数据库，通过子进程私有挂载使用；没有修改系统/用户证书数据库，没有忽略证书验证。没有访问或改动真实相机配置。

| 项目 | 实际结果 |
| --- | --- |
| 最终 Go 全量 | test PASS：signaling 126.757秒，simulator 0.003秒；race PASS：133.062秒、1.010秒；vet PASS，退出0且无诊断 |
| 编码离线 | 65536种 S16 与 FFmpeg 逐字节零差异；160B封包、单 credit、恢复时新采样、非8k拒绝；`cdp-fixture-green.log` |
| Chrome 生命周期 | 17/17 PASS，Go race；包含取消后迟到麦克风、迟到201精确删除、重复停止、无效票据、受控权限拒绝、真实WS握手黑洞、读包后关闭、主线程400ms卡顿不补发；7.47秒，包总8.479秒 |
| 真实详情按钮→SIP/C++ RTP | Normal PASS，3.316秒；60包、9600样本，PT8/SSRC/连续序号及160时间戳；数字1000Hz RMS 0.087404、归一相关0.999999；`web-detail-normal-integration.log` |
| 可信 HTTPS/WSS | Normal PASS，3.306秒；RMS 0.087404、相关0.999999；`web-wss-v2-normal-integration.log` |
| WSS + 路由退出 | Go race + 实际 C++ ASan/UBSan/LSan PASS；9600样本1.199264924秒，RMS 0.087006、相关0.995445；包总4.520秒；`web-wss-route-asan-integration.log` |
| 最终 WSS + 隐藏标签页 | Go race + 实际 C++ ASan/UBSan/LSan PASS；9600样本1.201460594秒，RMS 0.087006、相关0.995445；包总4.346秒；`final-web-wss-hidden-asan-integration.log` |
| HTTPS 生产 WHEPPreview | 实际 RTMP H.264/AAC 输入，Chrome 经 HTTPS 获取真实一次性票据及201/根相对Location，ICE/DTLS/SRTP后解码；视频14→59帧、音频24000→110400样本；活动resource GET=204，关闭后精确resource DELETE/再次DELETE=404；`https-playback-v4-browser.log` |
| 清理 | SIP BYE/200、owner结束、C++会话404、Chrome子进程退出；HTTPS播放夹具Go/C++退出0，FFmpeg受控TERM退出255、测试网关TERM退出-15，均属于本轮管理的进程 |

音频整链由真实 Chrome getUserMedia、AudioWorklet、WS、生产 Go 控制/SIP 和真实 C++ sender 完成；SIP/RTP 接收端是受控 peer，不是物理相机。音源为数字 WAV，60包全窗口减首100ms后按预声明1000Hz验相关，不挑选“较好片段”。共享 Go 测试夹具仍有历史“Qt capture”日志标签；本轮 Web 结果以 `TestTalkWebCaptureMedia` 测试名和 Chrome 输出识别，不表示重新执行了 Qt 采集。

C++ ASan轮启用 `detect_leaks=1:halt_on_error=1` 及 UBSan halt；保留 Boost.Context warning。浏览器、TLS网关和系统库未全量插桩。Go全量运行默认跳过需要显式真实服务/探针环境的集成测试，上表的显式整链运行另有证据，不用 SKIP 充当 PASS。本轮没有 C++ 生产修改，不把上一增量71/71或桌面9/9说成本轮重跑。

HTTPS关闭时生产播放器的DELETE得到404，符合资源已被会话终止路径清理的合同；证据不证明唯一清理触发器是HTTP DELETE，也不声称捕获了DELETE204。关闭前实际GET204与持续解码、关闭后404分别验证活动和消失。

## 原始失败及修正

- 首轮分段 G.711 量化在 S16 边界与 FFmpeg 不一致；第二种负值量化仍有差异。原始日志保留，改为对量化级二分取最近级，完整65536种输入零差异；不放宽断言。
- 首次私有 NSS 挂载未正确映射 `/dev`，Chrome启动 EACCES；修正本轮子进程的挂载参数后通过。不改系统 `/dev` 或放宽证书验证。
- HTTPS播放首次端口预检 bind失败，服务未启动；未证明是其他进程占用，TIME_WAIT也可能导致该预检失败。后续独立端口并按实际服务器socket使用reuse选项检查，原始失败保留。
- HTTPS探针v3把活动resource GET错写为200，生产明确返回204。仅修正证据探针为真实接口合同，v3保持FAIL，v4通过，不修改生产响应。
- 素材参数检查前先创建 Chrome profile 的泄漏有 RED/GREEN；把 profile 创建移至输入检查之后。缺失文件和目录输入均退出1且不产生 profile。
- 手动直接执行未设置 executable 位的运行脚本被 shell拒绝，改为明确 `bash` 调用；该次没有启动媒体进程，不算媒体失败。

## 复用与验证命令

离线测试（需要指定当前实际 FFmpeg CLI）：

```bash
FFMPEG_TEST_EXECUTABLE=/home/gyl/bin/ffmpeg node tests/web_console/talk_worklet.mjs
```

`TestTalkWebLifecycle` 及 `TestTalkWebCaptureMedia` 默认SKIP；真实测试需显式提供 `GB_AUDIO_TEST_WEB_PROBE`（可执行wrapper，参数为平台地址和受控设备ID）。整链另需 `GB_AUDIO_TEST_MEDIA_URL` 指向独立实际C++服务。wrapper使用既有CDP harness，显式 `WEB_CONSOLE_AUDIO_FIXTURE` 才启用浏览器数字音源；没有该参数不注入假麦克风。TLS fixture参数为 `GB_AUDIO_TEST_TLS_CERT/KEY`，仅加载本轮证书，不为测试绕过验证。可复核的完整命令、脚本和原始日志保留于证据根，不把测试密钥、token或原始地址提交源码。

## 自审及未完成项

只读独立复审未发现可达P0/P1。新增状态为实际资源、取消signal、真实在途HTTP/cleanup promise和Worklet封包/单credit；没有新增数据库字段、token消费副本、自动重试、通用音频工厂或媒体线程。

仍未验证：物理麦克风/相机扬声器及同步回传听音、Firefox音频采集、真实用户权限对话框操作、真实慢网络持续背压、设备拔出、页面真正销毁时的keepalive收尾、第二机Web对讲。它们为NOT_RUN/未完成，不由数字音频、路由退出或HTTPS播放自动推导PASS。真实双向声学与整个阶段六/平台仍未GO。

资源清理仅针对本轮：已删除四个自有构建目录的730个可再生成对象文件，共1,690,792,376字节；命令范围及文件清单保留于证据根，不删除源码、测试可执行程序、依赖前缀或其他Agent目录。后续只在核实归属后处理本轮 `/tmp`、`/var/tmp` 临时产物。

实现契约参考：[Web Audio](https://www.w3.org/TR/webaudio-1.0/)、[媒体采集](https://w3c.github.io/mediacapture-main/getusermedia.html)、[安全上下文](https://www.w3.org/TR/secure-contexts/)、[Chromium证书管理](https://chromium.googlesource.com/chromium/src/+/master/docs/linux/cert_management.md)。
