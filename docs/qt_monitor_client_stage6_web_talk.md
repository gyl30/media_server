# 阶段六：Web 麦克风与可信 HTTPS/WSS

本增量接续 `931d285` 的[Qt 采集](qt_monitor_client_stage6_qt_talk.md)，不替代[Go/SIP 对讲 owner](qt_monitor_client_stage6_talk_control.md)或 C++ 音频发送器。Web 只在父国标设备详情中通过用户点击启动麦克风；页面明确显示“正在上送麦克风，设备听音未确认”。观看画面的回传音频仍独立开启，不做独立广播 UI。

## 实现与所有权

- 单个 `TalkCapture` 拥有实际采集、AudioContext、Worklet、WebSocket、在途 POST 与精确会话 ID。没有另建 token 状态、重试、后台线程或关闭标志。
- 必须是安全上下文，申请单声道麦克风并使用浏览器原生 AudioContext 采样率。Worklet 先低通、连续降采到 8000Hz，再复用 PCMA 编码；不要求 Firefox 把不同采样率的采集源连接到 8kHz context。8000Hz 输入保留逐样本旁路；低于8000Hz或非有限采样率明确拒绝。当前实际验证率为44100/48000Hz，不据此宣称所有原生率都已验收。
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

## 原始实现验证（2026-10-17）

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

## Firefox 旧实现采集兼容性补验

2026-10-17 在 clean `ecc02068d8d14a802a9b31efcd7430f8fe52f0a1` 上补验 Firefox 142.0，独立证据为 DEV `/tmp/qt-firefox-capture-ecc0206-BxGF27rh/`。生产 `talk.js` 与 `talk-worklet.js` 的快照 SHA 与该提交工作树逐字节相同；没有修改生产或测试代码，没有将 Chrome 的结果推定为 Firefox 通过。

Firefox 使用本轮私有 profile 和显式 fake capture preference，不是物理麦克风。BiDi 指针点击实测 `isTrusted=true`、用户激活有效，loopback 页面为安全上下文。`getUserMedia()` 成功；8000Hz AudioContext 为 running，生产 Worklet 模块加载成功，但 `createMediaStreamSource()` 抛出 `NotSupportedError`：`Connecting AudioNodes from AudioContexts with different sample-rate is currently not supported.` 因而该旧实现的 **Firefox 142.0 麦克风上传为 BLOCKED**，不是 PCMA 或对讲整链 PASS；保留此原始失败，修复后的限定通过见下一节，不回写历史结果。

再用实际 `TalkCapture` 类复验同一失败路径：状态为 preparing→stopping→stopped，页面与静态 HTTP 服务分别观察到对讲 POST 数为0，麦克风 track 已 ended、AudioContext 已 closed、current owner 已释放。仅“预期不支持时的拒绝与清理”通过，没有进入 WS、SIP 或真实相机。没有为该限制增加第二套采样器、静默丢音频或关闭浏览器安全校验。

`result-production.json` SHA256 为 `d7af102f1b8ad6d3976ace0e7ae5408ac0782d3c05437388564a17e308835bf0`；`sha256sums.json` 保留页面、生产源码快照、三轮结果和日志清单。首轮键盘探针超时原样保留，不能算通过。结果中的 `firefoxExecutable=/usr/bin/dash` 来自 spawn 后、exec 前的启动包装器，不作为最终 Firefox 映像的证据；版本取自实际 `firefox --version`。本轮 Firefox 均由自己的 PID 收尾，HTTP 服务关闭，最终无本轮 profile 对应的 Firefox 进程，未修改 Pulse、用户 profile 或系统配置。

## 原生采样率修复与整链补验（2026-10-18）

接续基线 `f4773966635fafffbaa37192c905cc9c49df4af8`，仅修改 Web 采集两文件及其离线、浏览器生命周期测试；不改 Go 控制/SIP、C++ sender、Qt 或相机配置。旧8kHz context要求是在 Firefox142.0 上真实可达的兼容性缺陷，不通过忽略错误或丢弃音频解决。

Worklet 在非8kHz输入下只增加一个归一化 Blackman 窗 sinc 核、环形历史及分数相位。3.7kHz为低通中心，核支撑约10ms、群延迟约5ms；对相邻已低通时刻插值，避免44.1kHz每quantum取整漂移。不在实时process中计算三角函数；pending credit仍推进滤波、相位与包边界，恢复不会补旧完整包。该固定语音带限降采不是通用DSP框架、编码协商或新的音频队列；不增加服务器配置、token状态、重试、媒体线程或生命周期标志。

独立证据根：DEV `/var/tmp/qt-firefox-native-e2e-f477396-3QYG2kUX/`；离线RED原件另在 `/var/tmp/qt-firefox-native-f477396-KVJp0e/offline-red.log`。实际版本：Firefox142.0、Chrome153.0.8010.52、Node24.13.0；原有FFmpeg7.1.1 CLI用于编码参考，不冒充桌面FFmpeg9.0.1 SDK。

| 场景 | 结果与证据 |
| --- | --- |
| 离线RED | 退出1；旧Worklet对44100/48000Hz分别拒绝，8kHz全值域编码仍一致。不是改断言为假PASS。 |
| 当前源码离线GREEN | `offline-current.log`，退出0；8kHz全部65536个S16值与FFmpeg零差异；44100/48000Hz各10秒准确80000样本/500包，任意分块逐字节一致、包媒体时钟20ms、credit恢复新数据。1kHz相关度0.999950/0.999986；3.4kHz通带及4/4.1/6/10kHz离散阻带测试通过。不是全频扫频或实时性能基线。 |
| Firefox 原生采集→真实Go/SIP/C++ RTP | `run04/firefox-go-media.log`，PASS，3.737秒；真实用户激活指针点击、44100Hz context，60个PT8包、9600样本/1.208635078秒，1kHz RMS0.070390、相关0.999970。 |
| Chrome 同链回归 | `run05/chrome-media.log`，PASS，3.340秒；9600样本/1.204014274秒，RMS0.088542、相关0.999999。使用原有数字WAV夹具，不是物理麦克风。 |
| Chrome 生命周期 | `run05/chrome-lifecycle.log`，19/19，Go包7.624秒；包含新增采集图NotSupportedError时零POST、track/context结束，以及旧取消、迟到201、WS故障和400ms主线程卡顿。 |
| Firefox 生命周期 | `run06/firefox-lifecycle.log`，19/19，Go包8.609秒；同一正式生命周期探针，由私有BiDi适配器执行。卡顿后实际最大包间隔415ms、恢复150ms内8包，没有补发积压。 |
| Firefox 可信HTTPS/WSS + 后端ASan/UBSan/LSan | `run07/firefox-wss-race.log`，Go race包4.851秒；实际https/wss、44100Hz、9600样本/1.220048468秒，RMS0.070723、相关0.999928。 |
| Chrome 可信HTTPS/WSS + 后端ASan/UBSan/LSan | `run07/chrome-wss-race.log`，Go race包4.357秒；9600样本/1.213204566秒，RMS0.088138、相关0.995446，未放宽原0.90门槛。 |
| 最终Go全量 | `go-test.log`：signaling136.272秒、simulator0.003秒，PASS；`go-race.log`：143.146秒、1.012秒，PASS；`go-vet.log`退出0且无诊断。对应命令、真实耗时及退出码见`go-results.json`。默认SKIP的显式整链测试仍以表中单独执行结果计，不把SKIP充作通过。 |

Firefox音源使用自身显式fake getUserMedia测试偏好，预先声明接收端应为1000Hz；浏览器确实执行实际getUserMedia、AudioContext、Worklet、WS及生产详情按钮。Go独立受控设备执行真实SIP Notify/Response/INVITE/ACK/BYE，C++实际发RTP；接收60包连续序号与160时间戳后，除首100ms外其余8800样本全部参与原RMS/相关断言。不是物理相机设备、双向声学或第二机验收。共享测试中“Qt capture”历史日志标签未改，结果以`TestTalkWebCaptureMedia`及浏览器输出区分。

TLS证书来自原本轮有效短期测试CA，包含127.0.0.1 SAN；Firefox只向新建私有profile的NSS库导入该CA，Chrome沿用原私有NSS子进程挂载。没有忽略证书/主机名验证，没有改变系统或用户信任库。所有整链停止均验证BYE/200、owner.done、C++身份404及track ended/current释放；浏览器由自有PID结束，Normal/ASan媒体服务退出0。

复用的Normal媒体ELF SHA256为`45f0c48affb7b866578178f5298fe1291d6c6efbeab7f79c34366f54dda4fde4`，ASan ELF为`10bf9224b3a48892b26afb91b94e814a2ea1eb2ee8c2f38f78ba26b82ca4dd6a`，不是本轮重新编译当前HEAD的C++。C++路径沿用已验证音频实现，本增量没有C++差异；本轮未重跑全量C++/桌面CTest。ASan使用`detect_leaks=1:halt_on_error=1`及UBSan halt，完整保留makecontext/swapcontext警告；服务退出0、未报告内存错误。不将浏览器或系统库称为全量插桩。

原始测试工具失败同样保留：根目录首次Python3.10无`hashlib.file_digest`，未启动服务；run02私有BiDi顶层await错误，run03独立端口预检遇TIME_WAIT，run05在Chrome两项通过后，Firefox适配器不能求值分号语句。只修复仓库外探针并换独立未占用端口，分别在run04、run06、run07取得有效结果，不删除失败或改生产断言。

清理仅删除run04/06/07三个本轮成功、PID已消失且无活动命令行引用的Firefox profile，共52,983,859字节；保留user.js/prefs.js/NSS等配置快照、逐文件清单及全部结果/日志，失败profile未动。记录在`owned-profile-cleanup.json`，只涉及本轮`/var/tmp`，不是用户浏览器配置。原始数字WAV、C++运行包及旧失败证据保留。

## 复用与验证命令

离线测试（需要指定当前实际 FFmpeg CLI）：

```bash
FFMPEG_TEST_EXECUTABLE=/home/gyl/bin/ffmpeg node tests/web_console/talk_worklet.mjs
```

`TestTalkWebLifecycle` 及 `TestTalkWebCaptureMedia` 默认SKIP；真实测试需显式提供 `GB_AUDIO_TEST_WEB_PROBE`（可执行wrapper，参数为平台地址和受控设备ID）。整链另需 `GB_AUDIO_TEST_MEDIA_URL` 指向独立实际C++服务。wrapper使用既有CDP harness，显式 `WEB_CONSOLE_AUDIO_FIXTURE` 才启用浏览器数字音源；没有该参数不注入假麦克风。TLS fixture参数为 `GB_AUDIO_TEST_TLS_CERT/KEY`，仅加载本轮证书，不为测试绕过验证。可复核的完整命令、脚本和原始日志保留于证据根，不把测试密钥、token或原始地址提交源码。

## 自审及未完成项

只读独立复审未发现可达P0/P1。新增状态为实际资源、取消signal、真实在途HTTP/cleanup promise和Worklet封包/单credit；没有新增数据库字段、token消费副本、自动重试、通用音频工厂或媒体线程。

仍未验证：物理麦克风/相机扬声器及同步回传听音、真实用户权限对话框操作、真实慢网络持续背压、设备拔出、页面真正销毁时的keepalive收尾、第二机Web对讲。它们为NOT_RUN/未完成，不由数字音频、路由退出或HTTPS播放自动推导PASS。Firefox142.0旧8kHz实现的兼容性BLOCKED已由本轮原生率修复及限定整链验收解除，不推广到所有浏览器版本/设备或物理听音。真实双向声学与整个阶段六/平台仍未GO。

资源清理仅针对本轮：已删除四个自有构建目录的730个可再生成对象文件，共1,690,792,376字节；命令范围及文件清单保留于证据根，不删除源码、测试可执行程序、依赖前缀或其他Agent目录。后续只在核实归属后处理本轮 `/tmp`、`/var/tmp` 临时产物。

实现契约参考：[Web Audio](https://www.w3.org/TR/webaudio-1.0/)、[媒体采集](https://w3c.github.io/mediacapture-main/getusermedia.html)、[安全上下文](https://www.w3.org/TR/secure-contexts/)、[Chromium证书管理](https://chromium.googlesource.com/chromium/src/+/master/docs/linux/cert_management.md)。
