# Qt 公共对讲与真实大华并行回看

## 1. 本次范围与结论

2026-10-18，验证已有生产链：Qt SDL采集→FFmpeg PCMA→一次性WebSocket上传→C++音频session→真实大华RTP；同时用生产`monitor_window`持续解码摄像机H.264/PCMA。Go负责Broadcast/SIP对话及媒体控制续租，不由客户端直送摄像机。

最新`run06`的**限定数字链路及并行回看PASS**。不是摄像机扬声器、物理麦克风或完整双向声学GO。原严格五频/10dB声学试验仍为[INCONCLUSIVE](qt_monitor_client_stage6_stable_volume.md)，不以本次单频数字信号替代它，也不放宽旧门槛。

没有修改生产采集、编码、SIP授权、媒体传输或相机配置。本次只修正探针的首媒体/稳态分层、清理三个无生产消费者的Go返回副本，并修复独立浏览器测试的CDP归属问题。具体审计与测试见[遗留实现审计](qt_monitor_client_legacy_test_audit.md)。

## 2. 版本、机器和真实执行入口

- DEV：`172.20.45.187`，分支`feature/qt-monitor-client`，仅工作区`/home/gyl/example/media_server_qt`。
- TEST：`172.20.63.32`，只运行本轮独立服务，未占用其他Agent环境。
- 相机：Dahua `172.20.44.240`，设备`34020000001320000089`、通道`34020000001310000089`；保留原SIP平台指向，不写相机配置。
- TEST复用不可变runtime `/var/tmp/qt-device-seek-582bf95-k0lOi02X/runtime/`，57个manifest文件逐项核验；manifest SHA256 `59fc633766de8e5e9469403cdb1aa6315a2c2e89fe1ae3a4b89d5b1ef1118be2`。实际`media_server` SHA256 `45f0c48affb7b866578178f5298fe1291d6c6efbeab7f79c34366f54dda4fde4`，不能把该包来源`582bf95`重标成当前HEAD；`git diff 582bf95..15fb631 -- media`为空。
- run06 Go启动器在`15fb631`编译，使用仓库外Go test overlay启动真实SIP、HTTP、store和reconcile，没有mock SIP/静态RTP。启动器SHA256 `3ec20e67a2a3acc1ab1a5ac90cb76dfda35ae2df2139519cd55a21c3cefde983`。源码/overlay/构建日志都保留，不把私有启动器当成正式发行程序。
- Qt运行正常构建，probe新增首媒体计时后重新编译；`desktop_talk_probe` SHA256 `15c3c5895fc00fa6d702df8f3a6017fd3ae717ce69e42f826e3e7f35ce63d1d9`；`desktop_playback_probe` SHA256 `fe3d4dd5340146c4480103a5f9aa238e7b75f23c26e337a8040950f4dd9978c1`。采集和回看均调用真实生产组件。

Qt输入使用本轮私有Pulse Unix socket、独立48kHz单声道null sink和1kHz软件音源；不是物理麦克风。回看是Qt offscreen及SDL dummy；真实解码/图像提交成立，不等于显示器扫描或物理播放声音。此前安装版外部鼠标/XCB验证见[单独报告](qt_monitor_client_stage7_installed_play.md)，不混同本次probe操作。

## 3. 失败与修正记录

| 运行 | 真实结果与处理 |
| --- | --- |
| run01 | 系统tcpdump被注入仅属于媒体runtime的私有库路径，启动失败，未进行有效媒体验收。诊断工具改用系统环境，原失败保留。 |
| run02 | 控制凭据漏配，实际跨LAN内部verify被拒；Qt无帧提前结束，不能算并行回看。talk本地580、wire579包及独立回收仅是该子链证据。此前捕获未覆盖内部RTSP控制，拒绝归因有路由/源码/服务日志证据，不伪称完整wire抓到了这次403。 |
| run03 | 私有Go启动器使用无效`0.0.0.0` HTTP配置，生产校验正确拒绝；没有真实相机/音频验收，序列不变。恢复显式LAN地址并正确配置独立控制凭据，不放宽生产校验。 |
| run04 | 旧探针完整12秒PASS、并行Qt60.001秒/310项通过；wire596包可解码。但软件音源PCMA RMS/peak仅8，不能据此认定可用音量。owner观察器把合法异步清理短窗200/404判错；原FAIL保留，独立最终404/404及资源清理通过。 |
| run05 | 改用预声明25%软件音源并在POST前验证强度；wire571包数字强度通过、Qt60.000秒/310项通过，但旧连接锚点首秒仅21包，probe退出1，**仍记FAIL**。没有首包时刻，不能事后推断精确启动延迟或倒改PASS。 |
| run06 | 新版首包锚点：首媒体520ms后完整12秒，逐秒49–51包，probe退出0；wire600包，Qt60.099秒/315项通过，owner及全部资源回收通过。 |

Pulse首次启动的D-Bus名称冲突、旧FFmpeg4.4误判PS音轨和解码失败、监督器及观察器错误均保留；这些失败没有通过修改设备/生产协议、删除断言或替换旧结果来掩盖。

## 4. 首媒体和稳态不是同一阶段

生产`talk_session::transmitting()`在WebSocket连接并解除SDL暂停时发出，不承诺PCM已经到达；`send_audio()`允许当前输入不足，另有生产采集期限。旧探针却把连接时刻当成稳态开始。

新版从连接起独立限制5秒首媒体，首次真实`progress`才锚定完整2–30秒稳态。每秒至少25包、结束前末进展小于1秒、错误必失败、精确DELETE均保留。首包回调与timer均核对5秒；总预算32秒加观察时长。没有修改生产信号语义或新增生产状态。

run06实际：

```text
progress_scope: local_ws_enqueue
observation_anchor: first_local_ws_packet
first_media_latency_ms: 520
observed_ms: 12000
packets: 600
packets_per_second: [49,50,50,51,50,50,50,50,50,50,50,50]
probe exit: 0
process elapsed: 14.687443832983263 seconds
```

本地enqueue不能单独证明摄像机收包。本次独立wire恰为600，不据此推导所有运行都无损：run04/05本地与wire差1仍保留，不能仅靠这两个数字定位损失。

## 5. run06 独立抓包和数字音频

在公开POST之前先对私有采集源取8000个样本，预声明门槛：RMS128–2048、peak≤4096、1kHz相位不变相关≥0.8。实际RMS181.009658、peak259、相关0.999996487，通过后才发起talk。

冻结抓包独立复核：

- Broadcast Notify、独立业务Response OK、反向INVITE、200/ACK、公开POST201/WS101和DELETE204属于同一有效会话；BYE的两包是同branch/CSeq的重传，收到200，不算两个业务关闭。
- 线上600个PT8/160字节包；序号17861–18460连续、时间戳每包+160，PCMA/8000/mono。
- 按与preflight相同的未去DC公式，全96000样本raw RMS183.999710、peak248、相关0.999506948；首8000样本RMS183.996522、peak248、相关0.999497554，完整通过该1秒数字门槛。
- 20个实际UDP RTCP（18 RR + 2 SR），另20个ICMP3/3引用；没有实际camera RTCP。不能把ICMP引用内解码出的RTCP再算一次，也不能以本地SR或没有RTCP报告来证明设备收到并解码RTP。
- 摄像机回传PS有36863个连续RTP包；用既有n7.1.1 FFmpeg离线解出661920个PCMA样本/82.74秒，1379帧×480样本。最大相邻PTS残差60ms、5条非单调DTS诊断保留，不以工具退出0宣称连续精密音频时钟或声学关联。

run06 pcap SHA256 `c85bbaa38e6ef2caecfd6852e5e2a9ac52e4633af2c327846c53dd46e62af959`，DEV/TEST一致。独立派生摘要SHA256 `3c9098735bf63048183c3821c922011d1e18529c6ef905e752f3644f924cfad4`。

## 6. 并行回看、所有权及清理

Qt真实H.264 1920×1080/25fps和PCMA回看在共同60.099秒窗口完成315/315项断言；窗口完整包含talk进程及其RTP活动。不是先后串行的两次观看，也不是仅根据网络字节判断。数字音频、画面进展和最终线程/QObject释放详见`camera-playback.private.json`及独立摘要。

公开owner观察70次，从signaling200/media204到两侧404；允许现有异步逐层关闭的短暂不同步，但要求5秒内两侧同时结束。不通过等待无限清理或新增closing状态获得结果。

TEST服务总144.327783秒，Go、media和tcpdump按预定、有身份核对的关闭流程退出0，无强杀；Go由停止文件结束，尚存media/capture分别接收SIGTERM/SIGINT，不能称三者自然EOF。Qt talk/play完成观察后退出0，私有Pulse退出。逐项PID/starttime、exe、PGID核对后确认消失，receiver/live/broadcast归零、原监听端口可重绑。本轮未修改sysctl、全局limits、用户桌面或系统Pulse。

广播SourceID持久高水位延续，不回滚旧DB：run04为12→13，run05为13→14，run06为14→15。最新数据库DEV/TEST SHA256均为`6fe84a5d615684416ebdee851d17a90df30eeb7aafedd6cf347db7e6fafd842d`。后续实验必须从最新15延续，不能复用较早的11/13。

## 7. 证据路径与安全

| 运行 | DEV | TEST |
| --- | --- | --- |
| run01/02 | `/var/tmp/qt-public-talk-2bb192a-TWTq2AVW/`（run02在`test-run02/`） | run01 `/var/tmp/qt-public-talk-2bb192a-yBhrKgNa/`；run02 `/var/tmp/qt-public-talk-2bb192a-run02-gqyq1fbF/` |
| run03 | `/var/tmp/qt-public-talk-2bb192a-run03-XnRcISuT/` | `/var/tmp/qt-public-talk-2bb192a-run03-aNOIh37j/` |
| run04 | `/var/tmp/qt-public-talk-2bb192a-run04-yV6lrmew/` | `/var/tmp/qt-public-talk-2bb192a-run04-4G28A27M/` |
| run05 | `/var/tmp/qt-public-talk-2bb192a-run05-45TzYmvv/` | `/var/tmp/qt-public-talk-2bb192a-run05-KMjY3XhS/` |
| run06 | `/var/tmp/qt-public-talk-first-media-run06-rzpaiZCh/` | `/var/tmp/qt-public-talk-first-media-run06-xOdJxyAF/` |

各DEV目录按实际执行阶段保留私有脚本、stdout/stderr和进程身份；run04–06有Qt统计及`test-evidence/`中的TEST冻结副本，音源preflight只在run05/06执行，不能给未进入媒体阶段的run01/03补造这些产物。run04/05/06各有`independent-wire-audit.json`及独立离线解码材料。run04摘要SHA `ead81c045ccc8a65d4ac5df812c18060cdb3b3913358d73aa1073a5c54445613`；run05摘要SHA `b9424a99eeca7c3594b0afadf809f434ac8adc04b3071fdfe53f6087db54f638`。

凭据、原始SIP/URL、控制token、pcap和DB均留在仓库外私有目录，不提交。未删除失败记录或原始性能/设备证据，也不复制未脱敏报文到本文。

## 8. 仍未闭合的验收

1. 摄像机真实扬声器/物理麦克风及完整双向声学仍未证明；旧严格比较仍INCONCLUSIVE，不能给阶段六或整个七阶段GO。
2. 相机RTCP未观测到；ICMP端口不可达的事实保留，不为本轮新增兼容、重试或改变RTCP策略。
3. probe无首媒体/中间单秒停滞尚无独立稳定注入行；早断和正常行不能冒充这些负对照。
4. 本次实际设备运行是Normal；ASan的相关组件测试和先前受控真实整链有独立证据，不把它们合称这次相机ASan运行。
5. 内部搬运包、16路不同源和已有观看结果各有独立范围；公开分发许可、完整PTZ及物理声画限制不因本次数字链PASS消失。

结论：可继续使用现有公共中继对讲路径做限定LAN数字功能验证，没有由本轮证据确认的生产P0/P1；不建议宣称完整声学验收或公开产品发布GO。
