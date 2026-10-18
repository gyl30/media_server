# Qt公共对讲探针的有界观察

2026-10-18，在`e870f61`之上仅扩展测试探针、现有测试及构建依赖。生产Qt对讲、Go授权/SIP、媒体发送和生命周期均不改。

## 原因和实现

现有`desktop_talk_probe PLATFORM DEVICE_ID`在上传建立后观察2秒。这个默认保留，既有Go生产媒体组合仍原样调用。新增末位可选`SECONDS`，只接受2–30整数；真实设备首轮计划12秒，30秒上界容纳后续控制续租观察，不是无限采集或容量工具。

原`2bb192a`版本从`transmitting()`开始使用单调时钟；记录每秒累计值、实际停止前观察时长、最后进展和结束时刻。每个完整秒桶至少25个本地160字节WS包，末进展距观察结束小于1秒，全部请求时长完成才可通过。25包/秒沿用旧2秒至少50包的功能进展下限，不是完整50Hz收包率承诺。秒桶独立于观察/停止timer先后顺序，避免最后一秒漏检；DELETE时间不计上传观察时长。原总期限27秒加请求时长，覆盖既有POST15秒、WS5秒、DELETE5秒和少量调度余量。仍等生产`finished()`后退出，不用强杀充当成功。

输出明确标记`progress_scope=local_ws_enqueue`：它只证明SDL采集→FFmpeg编码→Qt WebSocket接受160字节，**不证明设备收到、解码或扬声器播放**。真实设备必须再独立核对协商、出口RTP和回传媒体。

## 验证

- Normal构建成功；`desktop_talk_session` 1/1 CTest，17项QTest、零失败/跳过，21.83秒。
- ASan+UBSan+LSan构建及同组1/1、17项QTest零失败/跳过，21.96秒。`detect_leaks=1:halt_on_error=1`、UBSan halt/stacktrace，实际ELF加载libasan/libubsan，无抑制或环境守卫绕过；FFmpeg/Qt等外部库未全量插桩。
- 新增六行：默认2秒、明确12秒、上传早断、0/31/文本非法时长。真实子进程与原Qt HTTP/WS fixture验证退出码、单次创建/连接、精确ID DELETE与真实静音PCMA。早断必须失败且观察未完成。原取消/超时/窗口关闭/切平台覆盖保留。CMake增加必要probe构建依赖，原30秒CTest期限不改。
- 原`TestTalkQtCaptureMedia`仍两参数默认2秒：最终Normal 2.294秒、ASan 2.305秒，均退出0。原受控SIP peer分别实际抽验60包/9600 PCMA样本、PT8/SSRC/sequence/160时间戳与静音内容、正常BYE及媒体owner删除；Qt各上传98包，两秒桶47/50，观察2000ms。不是大华或物理麦克风验收。
- 最终两个组合监督器均PASS，自有媒体进程退出0、PID/进程组消失、TCP端口按生产`SO_REUSEADDR`契约可重绑。

首轮私有监督器在媒体测试已通过后，因无`SO_REUSEADDR`的即时TCP重绑检查报Errno98，且未写出结果JSON；不能记为完整清理PASS。另存修正版Normal通过，但随后ASan预检同一缺口又在启动前报错，没有启动新进程。最终版补齐预检契约并保留两份失败脚本、原日志和解释，不更改服务端或掩盖失败。

## 证据与边界

DEV `/var/tmp/qt-talk-duration-e870f61-qE5OSnqV/`：构建、`normal/asan-ctest.log`、17项详细`*-last-test.log`、`source-and-probe.sha256`、插桩依赖。最终组合为`real-default-fixed-normal/`与`real-default-final-asan/`，含原始日志、身份及`result.json`。首轮失败为`real-default-normal/`和`first-default-wrapper-failure.txt`。

Normal probe SHA `597ec75de66c73e129949446f7354542ca5dd270d466a5331d6ba3261e368de0`；ASan probe SHA `81e657c4eaa1d411404485ecbed4366fb13b3533398b2780b7a2bb837cb2dad2`。

未跑整个桌面全套、Go全套或真实相机，不能据此宣称阶段六完成。连接存活但特定秒停滞尚无独立故障注入行；当前秒桶明确否决该条件，早断回归不能冒充该负对照。仅测试中新增有界计数/观测，不新增生产状态、endpoint或采集实现，也未删除历史覆盖或证据。

## 首媒体准备与稳态分离

后续真实设备`run05`的原始探针结果为FAIL：首个连接秒桶21包，随后11桶均50包，正常停止且无生产错误。证据保留在DEV `/var/tmp/qt-public-talk-2bb192a-run05-45TzYmvv/talk.private.stdout`。旧版没有记录首包时刻，不能据此确认具体是Pulse启动延迟，也不能把它倒推为PASS。

源码`talk_session`在WS连接后解除SDL暂停并立即发出`transmitting()`，该信号不是首PCM已准备好的承诺。探针现将首次本地WS包与连接建立分开：从`transmitting()`起单独限定5秒首媒体期限，首次`progress`记录`first_media_latency_ms`后才开始请求的2–30秒稳态观察；超时及首包回调均检查该期限。JSON新增`observation_anchor=first_local_ws_packet`和`first_media_deadline_ms=5000`，未获得首媒体时延迟字段为null。

稳态仍逐秒要求至少25包，结束前最后进展小于1秒，没有降低门槛。`observed_ms`、`last_progress_ms`和观察项`steady_ms`统一使用首包锚定时钟；原`transmitting_ms`继续表示真实连接后经过时间。首包本身计入第0桶；未到首包或未完成全部稳态窗口均不能通过。总期限增加明确的首媒体预算，改为32秒加请求时长。生产采集、编码、上传、关闭和DELETE路径不变。

已有真实子进程回归补验观察锚点、首媒体延迟、稳态末进展及观察字段，原正常/早断/非法参数和精确DELETE断言保留。本增量的构建与运行结果须另记，不能沿用上文旧版PASS作为本版结果；无首媒体和中间单秒停滞仍无独立稳定故障注入，不新增生产测试接口或虚报覆盖。

本版补测：Normal定向`desktop_talk_session` 1/1、17项QTest零失败/跳过，21.92秒；ASan+UBSan+LSan全套中的同组17项零失败/跳过，22.18秒（此时其他组仍在执行，不提前宣称整套PASS）。日志在DEV `/var/tmp/qt-final-audit-2bb192a-duu1a2Of/first-media-normal-LastTest.log`及`first-media-asan-in-full-ctest.log`。首条Normal构建命令把目标误写为`desktop_talk_session_test`，保留失败日志；正确目标`talk_session_test`构建及测试成功，未改CMake绕过。

真实大华新版`run06`首媒体520ms后完整观察12000ms，12桶均49–51包、600次本地enqueue和600个实际出口RTP，正常退出。另一个真实Qt60.099秒回看窗口完整包容talk并完成315项断言。独立wire数字门槛、历史失败、RTCP限制及资源回收见[公共对讲实机报告](qt_monitor_client_dahua_public_talk.md)；仍不是物理听音、五频声学或整个阶段六GO。
