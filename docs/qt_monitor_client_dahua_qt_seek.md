# 大华同会话定位的 Qt 实机增量验证

## 1. 范围与结论

2026-10-18，在 `feature/qt-monitor-client` 的 `582bf95f07dd8519cc515f0191238ebf4b784779` 上验证：

```text
DEV 当前 Qt playback_view::play_record / seek_record
  → 只记录 HTTP 收据的私有转发器
  → TEST 当前生产 Go playback API / SIP dialog / INFO
  → 真实大华摄像机
  → TEST 当前生产媒体服务器的有限 PS HTTP
  → DEV 当前 Qt FFmpeg 解码 / QWidget 呈现 / SDL dummy 输出
```

最终 Normal 与 ASan+UBSan+LSan 各一轮，分别23.602秒、23.376秒，进程均退出0，各14项断言全部通过。两次定位后的实际音视频解码 PTS 均发生显著前跳/回跳，呈现及非零 PCM 继续增长；停止后原 decoder 与 history owner 真正析构。

这是当前 Qt **播放组件的真实设备链路**，不再只是旧私有 collector 或受控 HTTP fixture。不新增生产 getter、状态、接口、重试或 codec flush。没有执行真实主窗口按钮/录像目录窗口的鼠标操作，也不是物理屏幕、扬声器、音画精确同步或精确落点验收。Scale 尚未实现客户端双轨适配，完整阶段四及整个平台仍未 GO。

## 2. 环境、输入与权威版本

- DEV `172.20.45.187`，Ubuntu22.04 / Linux6.8.0-136 / x86_64；GCC16.0.1 experimental、Qt6.2.4。Sanitizer运行库解析到独立GCC16.2安装，不把编译器写成16.2。
- TEST `172.20.63.32` 仅运行 DEV 构建产物，无源码开发或系统配置修改。启动前约80GiB可用内存、swap占用0、`/var/tmp`约52GiB可用；本轮没有容量测试。
- 相机 `172.20.44.240`，设备`34020000001320000089`，通道`34020000001310000089`。Web只读查库存，随后真实 GB RecordInfo 再确认；未改相机配置。
- 录像为设备本地时间 `2026-10-17 14:44:25–14:45:28`，UTC+8，对应`1792219465–1792219528`；H.264及PCMA。请求定位为原范围相对秒数30、5，不依赖当前显示PTS推导。
- 媒体服务器从本工作区的既有独立构建目录重新构建，退出0；Go从同一HEAD执行`go build`，退出0。私有runtime manifest准确记录HEAD、clean状态、ELF及非系统依赖SHA，TEST启动前逐项核验。
- Qt链接已通过全量回归的当前`libdesktop_media.a`，Normal和ASan分开链接；实际shared FFmpeg SDK为`/var/tmp/qt-broadcast-sdk-755cf52-z2y4wEha/install`，不是媒体服务器的另一份静态SDK。
- 本次再次核对上轮八份编译输入和六个正式Qt/测试ELF的SHA均不变。此前同一源码的Normal/ASan桌面CTest各9/9、各449项QTest及0跳过仍只作为其原受控回归，不伪称本次重新执行全量CTest。

## 3. 私有探针与观察方法

仓库外`seek_probe.cpp`直接链接当前桌面静态库，使用实际`play_record()`及`seek_record()`，不自行构造重复控制请求。首帧/PCM就绪后先观察，依次请求30和5，持续100毫秒采样，并最终调用现有stop。没有缩短或修改生产期限。

- 在第一次seek前只保存一次原decoder和history owner的QPointer，之后不重新捕获；每次检查当前唯一对象仍为原对象。
- 分别记录decoded PTS、解码/呈现计数、现有缓存图像SHA和PCM计数。它们是不同事实：图像/hash及计数不是“某个PTS对应图像已经物理显示”的证明。
- 链接器`--wrap=SDL_QueueAudio`先原样调用真实SDL函数，只在成功后统计排队数据，未替换音频输出为假成功。记录非零S16样本数、峰值及RMS；未静音降级，队列错误0。
- 私有API转发器只转发原请求并记录上游状态，不修改body或票据；PS媒体GET仍直接连接TEST。其响应时间戳在写回Qt之前，不能当作Qt收到204的时间，更不能据此划媒体边界。
- Qt原状态提示204仅表示受理。此次没有把HTTP完成后的首帧当新段，也没有将采样PTS当绝对录像时间。

## 4. 最终实际结果

下表是对应phase首次与末次100毫秒采样之间的计数，不是整段录像完整性或性能窗口。控制后仍可能先消费已缓冲的旧媒体，不能把phase内每个样本都算作新位置。

| 配置 | 控制后的采样phase | rendered首→尾 | 重采样PCM首→尾 | SDL非零S16样本首→尾 |
| --- | --- | --- | --- | --- |
| Normal03 | 前跳请求后 | 93→230 | 201504→460608 | 363092→873962 |
| Normal03 | 回跳请求后 | 232→448 | 475008→878112 | 879614→1691082 |
| ASan03 | 前跳请求后 | 94→224 | 204384→463488 | 368788→857166 |
| ASan03 | 回跳请求后 | 226→445 | 463488→880992 | 874060→1685620 |

解码器PCM计数为48kHz每声道样本数；SDL观察器统计stereo交错的S16值数量，二者分母不同，不相减或当作音频丢失率。

- Normal03检测到视频`8.590→29.880秒`、音频`8.630→29.880秒`，随后视频`35.000→5.290秒`、音频`35.040→5.330秒`。
- ASan03检测到视频`8.950→29.200秒`、音频`8.990→29.200秒`，随后视频`33.440→4.760秒`、音频`33.480→4.780秒`。
- 以上均为解码侧100毫秒采样的实际PTS跳变；包含设备定位/解码缓冲和采样误差，不能用它计算精确定位误差。采样到前跳后，至第二请求前的观察较短（Normal约700毫秒，ASan约301毫秒），不冒称长时新位置稳态；回跳后的对应窗口约3.7/4.199秒。
- Normal整轮SDL成功排队1715904个交错S16值，其中1691082非零，峰值135、RMS19.403975；ASan为1710144、1685620、135、19.614210。信号较小但确有非零，不等同设备麦克风声学质量或扬声器听音通过。
- 两种模式均无`playback_error`或decoder错误，原decoder/owner保持且最终销毁；各14/14。真实PS回跳产生FFmpeg的`DTS ... out of order`诊断，原stderr保留；未以屏蔽告警或退出0替代媒体与生命周期断言。
- ASan03实际运行环境为`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`、`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`、`LSAN_OPTIONS=exitcode=23`。runtime maps确认加载libasan及当前shared FFmpeg；日志无ASan/LSan错误和UBSan runtime error，退出0。Qt/SDL/FFmpeg系统shared库未全部插桩，不扩大依赖验证范围。

## 5. 真实协议收据与停止

抓包只覆盖SIP15060及媒体HTTP38472，不覆盖相机媒体TCP端口；不能用这份抓包证明相机RTP连接/完整包序列。独立解析的脱敏收据保存在证据目录的`protocol-receipts.json`，对应API轮次关联、Call-ID摘要、CSeq及HTTP连接。

最终两轮各只有一个播放创建201和一个PS GET/HTTP200，同一SIP dialog中两次Range控制均得到外层SIP200及内层MANSRTSP200，Qt原owner发出DELETE204；INVITE/ACK和BYE/200完整。线上每轮实际有3个INFO请求包，包含UDP重传；只能说“两次控制事务”，不能说“只有两个INFO包”。两次事务都未返回RTP-Info，不承诺精确边界。停止时Qt主动以RST关闭PS HTTP连接，不写成FIN优雅收尾。Normal03对应私有API收据13–16行，ASan03为17–20行，各为201/204/204/204。

结束后TEST控制接口`/receivers`明确为空；media/signaling均退出0、核对PID消失，SIP15060及38470–38473重新bind成功。本轮私有API转发器也已按PID/starttime核对后停止。没有停止其他进程或修改相机、全局网络及资源限制。

数据库来自上一轮已退出平台的只读SQLite backup，原高水位11保持。旧DB SHA前后相同；本轮不调用广播、不分配新SourceID，不回退到更旧DB。

## 6. 保留的失败与修正

1. 库存初查的私有脚本未处理当前日期无匹配时相机返回400，退出1；finally已销毁finder。独立第二目录补HTTPError记录后，三天查询和finder释放全部退出0，真实库存未改。
2. 初次探针CMake缺独立QtWebSockets路径，配置失败；显式使用此前桌面相同前缀后构建通过，不安装或替换系统Qt。
3. `normal-01`结果FAIL、退出139：私有探针对没有Q_OBJECT的platform_client使用Qt静态元对象筛选，误识别QNAM为owner，且退出时view的状态回调访问已销毁的探针局部结果。保存v1源码及原结果；改为直接QObject子项的C++动态类型检查、退出前断开观察回调。不是生产RED，不改业务代码。
4. `normal-02`、`asan-01`媒体/退出均通过，但初版QPointer可重新捕获，不能单独证明原对象贯穿；最终探针只在第一次seek之前捕获一次，Normal03/ASan03重新执行并通过严格断言。
5. `asan-02`启动器忘记将准备的env传给Popen，Qt尝试没有DISPLAY的XCB而SIGABRT，未执行媒体。该轮metadata的environment是意图值而非实际继承值；保存启动器v1、日志和退出-6，修正实际传env并在独立`asan-03`运行。不能将该轮当作ASan内存失败，也不能算PASS。

不删除或覆盖以上失败，没有为通过而放宽生产错误、关闭或媒体规则。

## 7. 证据、清理与剩余范围

- DEV：`/var/tmp/qt-device-seek-582bf95-3AQLTXJs/`。
- TEST：`/var/tmp/qt-device-seek-582bf95-k0lOi02X/`。
- 最终结果：`seek-normal-03.json/log`、`seek-asan-03.json/log`、`seek-asan-03-process.json`、runtime maps、`summary.json`、`probe-inputs.sha256`、runtime manifest、完整pcap/API私有收据与`test-evidence/cleanup-final.json`。原始票据、日志、抓包及finder结果均为私有资料，不能未经脱敏公开提交。
- 冻结pcap SHA256：`c2c093e7c3753357f7b79e68427ec788fd766759b00fe6ed1cbfd390df966d63`；采集报告27547包、内核丢包0。`summary.json` SHA256：`06cd44c6cbbc85cbc3faa5e5dc74e49e858f291614ae13031363f8e0a1ef7bf8`。完整清单另见证据目录`SHA256SUMS`。
- 独立最终收据`protocol-receipts.json` SHA256：`24cf286a042abbeb0ffb57085823df52bec44a7756e9dbcdaff75f8975ff5047`；配套Markdown SHA256：`233710262fff7b8274e7c84d0ca6b9aa1222c9d303098fac4a749f611b72d62d`。较早的`independent-seek-protocol-receipt-01/02`只是中间分析，最终引用本收据。
- 只删除本轮DEV `test-evidence/runtime`的冗余副本：逐项SHA与原DEV runtime一致、本轮服务已退出且核对无进程命令行引用该副本，文件总345727352字节（约329.7MiB）。原DEV和TEST runtime均保留，未丢失唯一源码/ELF、原始媒体或失败证据；见`owned-cleanup.json`。不动归属不明目录或`/tmp`、`/var/tmp`以外的资产。

仍未完成：Qt真实设备主窗口鼠标交互/物理声画、不同NVR、完整定位误差与设备忽略行为、Qt实际倍速双轨呈现，以及七阶段计划中的其他未闭合项目。当前固定1×音频与有界消费会反压AVIO；不能把请求倍率或受反压的解码PTS/墙钟斜率直接当设备实际倍率。下一步先建立独立可证伪的输入节奏观察，再决定必要呈现元数据，不静音、不无界缓存、不新增通用状态机。
