# Qt 监控客户端阶段二验证

## 范围及状态

接续阶段一 `095953318d7345b8c7890d77f205a05e690c7bbf`，仅在 `feature/qt-monitor-client` 开发。阶段二开发及以下定向验证完成，不代表后续阶段或完整产品 GO。

传统浅灰监控外壳保持不变。窗口直接持有16个独立 `playback_view`，使用原有平台授权及 RTSP/TCP 解码，不增加私有媒体协议或另一套媒体分发。支持1/4/9/16布局、点击选择、双击设备及拖放到目标画面、停止单画面/全部、右键布局、全屏/Escape、保持比例、PNG抓图。抓图保存当前显示分辨率，而非宣称原始码流分辨率。QSettings仅保存平台地址、布局、比例及正常窗口位置；不保存票据，不恢复自动播放或声音。

## 状态、资源与关闭

- `selected_` 指向真实画面，是操作路由的唯一事实；view的 `selected` property仅派生绘制边框。
- 缩小布局停止并隐藏越界画面，选中越界时回到第一格；可见的其他会话不重建。
- 窗口首次显式开启声音才创建唯一 SDL 设备，各view临时借用；切声先暂停/清空旧队列并清旧借用，再让新view使用同一设备。不反复关闭重开，不混音。
- 删除view原 `audible_` 字段，声音状态从借用的非零 `audio_id_` 推导。stop/EOF归还设备；窗口析构先停止全部view、解除借用，再Close设备，main最后SDL_Quit。停止或替换播放默认静音，新播放由用户显式开声。
- decoder尺寸更新复用既有 `output_size_` 和 `mutex_`。GUI只传QSize，解码线程不读取QWidget；允许已经转换中的一帧使用旧尺寸，不新增尺寸代际/清队列协调。
- 原 session QObject 隔离、取消/超时、自然关键帧、有界视频及PCM队列继续使用；不增加 started/closed、音频manager或播放槽位状态机。

## 主动 Catalog

新增 `POST /api/devices/{device_id}/catalog`，复用原在线注册、SN、pending Catalog 和 SIP MESSAGE 实现；校验设备存在、在线且非删除中，网络请求5秒期限，不持live锁等待网络。失败清理该请求的pending，SIP接受返回202及SN；**202不代表异步目录响应收齐**。客户端提示稍后手动双击设备刷新通道，不添加无限查询/重试。

真实 GB 模拟器验证自动REGISTER目录一次及手动两次查询，SN2→3，实际目录响应计数1→3，无INVITE/live，两进程退出0。证据：DEV `/var/tmp/media-server-qt-stage2-GwavyowG/catalog-simulator/`。模拟器不是物理摄像机目录验收。

## 已执行自动回归

| 场景 | 结果及边界 |
| --- | --- |
| 窗口布局/选择/拖放/取消/偏好 | 先旧实现RED，再新实现14项PASS；包含旧GB切平台回归及6类非法拖放 |
| 播放与借用设备 | 24项PASS；dummy设备验证旧stop/重复stop/析构/真实RTSP finished不影响新借用者，暂停状态下精确队列19200bytes；不是实际声音输出验收 |
| decoder尺寸 | 11项PASS；包含动态尺寸并发及非法尺寸，保留原轨道/取消/超时回归 |
| Catalog HTTP/SIP | 两组9个子场景PASS：真实UDP200/403、期限/取消及非法/未知/离线/过期/删除中 |
| Go全量test/race/vet | 最终串行PASS：test主包18.478秒/simulator0.002秒，race主包19.260秒/simulator1.008秒；GOMAXPROCS=2、-p1、不放宽断言 |
| 最终正常/ASan+UBSan CTest | 最终代码串行重建，分别4/4 PASS、41.33/41.59秒；两配置各61个QTest passed、0 failed、0 skipped（包含init/cleanup） |

QTest数量包含init/cleanup。窗口及借用设备RED/GREEN在阶段一证据根的 `stage2-window-*`、`stage2-view-audio-*`；decoder/Catalog在 `/var/tmp/media-server-qt-stage2-GwavyowG/`。原有Go SIP mock的UDP引用计数warning可由既有 `TestConcurrentLiveReusesOneUpstream` 单独复现，实服务目录验证无该warning；未隐藏日志或修改第三方依赖。

## 真实监控窗口16路

证据：DEV `/var/tmp/media-server-qt-stage2-main-7Do5ny4N/monitor-16-1080p25-01/`。使用现有探针新增 `--monitor-window` 模式，创建真正的 `monitor_window`，等待实际设备树，再通过画面鼠标事件及树双击信号启动各画面；不是另造16格探针窗口，也不是16个静态黑框。该模式是自动功能测试，不冒充对独立发布版ELF的外部鼠标操作验收。

硬件 Intel i7-13700KF（24逻辑CPU/16物理核、31GiB），Linux x86_64；GCC16.0.1、Qt6.2.4、SDL2.0.20、独立FFmpeg9.0.1 shared SDK，逐路 `thread_count=1` CPU软解。私有Xvfb、loopback隔离服务，非跨机器网络容量。媒体固定 H.264 Main Level4.0、1920×1080、25fps、GOP25、无B帧、视频CBR目标4Mbps；AAC48kHz双声道128kbps。素材20秒循环，30秒观测跨越循环边界。素材SHA256：`717951f2a7e2c49eab30f44a4a669d3c80318bc52a134dfb7f3f12eb64acfcb9`。

**PASS：同一源的16个独立授权、RTSP播放及解码会话。** 不是16个不同视频源。所有画面实际双轨就绪后共同窗口30.000秒，2040项断言0失败；每路749帧解码/749帧交付view（标称99.87%，交付率100%），每路1,440,768个48kHz PCM样本（100.05%，包含AAC帧量化）。各路视频和音频PTS逐秒增长；首帧均2.734秒；停止100ms后解码线程及对象释放。

计数中的“交付view”不是硬件垂直同步/屏幕扫描计数；就绪截图仅证明真实布局及图像，不能替代整个观测窗口。音频全部实际解码，只一路送输出；另行实录声音，不把PCM计数当作声学输出。

1秒资源采样33次、实际跨度32.182秒，含启动及观测，不是隔离稳态性能基线：进程CPU平均2.182核、GUI线程0.0283核；采样峰值RSS300.20MiB/PSS256.55MiB、FD53、线程22、该进程swap0。CPU分母为实际采样跨度，峰值受1秒采样精度限制。宿主原有swap存量不等于本轮发生内存压力，采样时CPU/内存PSI avg10为0；未改全局设置或停止其他进程。

前置四画面6秒真实GUI烟测也通过，保留于 `monitor-quad-smoke-01/`。完整命令、私有日志、逐秒媒体、资源JSON、分析脚本、截图和退出码均保留；原始日志可能含一次性URL，不提交Git或公开发布。

本轮16路探针ELF SHA256为 `13af84354dcf3cb0e21230927f7f0366882f027c89a8ee0845e68ba234a3ce48`；随后仅测试程序增加“共同观测完成后停部分画面”模式，GB轮使用 `592e109abdde23b76cc62f39708b870ade078daef8816ceb726401067db9f59b`。不能将后者SHA冒充原16路测试所用ELF。

## 同GB通道共享及停部分画面

`/var/tmp/media-server-qt-stage2-GwavyowG/gb-monitor-quad-cold-phase1-02/`：独立真实模拟器、SIP平台、媒体服务和监控窗口，H.264/PCMA冷首开。显式使用已验证的单设备 `--phase-buckets 1`，193项断言0失败；四格首帧均7.943秒，共同10.000秒每路解码250帧、交付view249帧、480000 PCM样本、0队列丢弃。然后实际UI停止前三格，三线程销毁，最后一格继续3秒增长75帧/144000 PCM样本。

四张独立授权和四次RTSP DESCRIBE，共享唯一INVITE/ACK上游。全部viewer退出后BYE仍为0、live仍为1；显式删除live才BYE并释放receiver。最终shutdown100ms，所有本轮4个进程回收，监听及媒体端口实际bind复核空闲。模拟器不是物理摄像机，不能把本项扩大为设备动作验收。

## 实际两音源切换及关闭

DEV `/var/tmp/media-server-qt-stage2-audio50-VlVrz8/run-03/`：独立实际 `gb_monitor` ELF、Xvfb/XTest，两个真实H.264/AAC源分别带440/1000Hz。50次显式选画面并点击声音，私有Pulse数字回环实录；不是dummy设备或probe内部音频计数。每次目标音调数字观测延迟104～150ms，最长连续静音80ms。该延迟包含PCM文件到达、轮询及100ms FFT窗口的观测成本，未扣除采样链路延迟，不是音频设备内部或声学起声时延。55段稳态的1322个频谱窗最小隔离28.24dB；预先固定阈值为750ms观测、500ms静音、20dB隔离，未根据失败放宽。

订阅及采样共同确认仅创建一个sink-input、同ID复用且同时最多1个，正常关闭后为0。停止当前、替换默认静音、缩布局停止声音owner、重新扩展不自动开声、停止全部及WM_DELETE正常退出0均通过；全部本轮10个子进程回收。数字回环不等同物理扬声器/声学时延。程序SHA256：`6c61b14d00e32b99d353e4d96fe35b5809f95225e3f3598ebfe41e6c671b347f`。

## Sanitizer 与内部搬移包

独立 `desktop-asan/` 使用GCC16.0.1、`-O1 -g -DNDEBUG -fsanitize=address,undefined -fno-omit-frame-pointer`，实际链接libasan/libubsan，无ASAN/LSAN/UBSAN suppression或禁用泄漏检查。桌面不依赖Boost.Context，没有绕过服务端守卫。Qt/SDL/FFmpeg等外部库不是全量插桩，不把项目代码通过扩张为所有依赖内存安全证明。

除完整4项CTest外，`asan-monitor-quad-aac-01/` 真实四路RTSP H.264/AAC、监控窗口及单路私有音频输出，双轨共同窗口10秒，200项断言0失败，first-frame2.824秒、shutdown99ms、对象释放通过，进程退出0；无sanitizer诊断。这不是ASan容量测量。

最终内部包：`/var/tmp/media-server-qt-stage2-main-7Do5ny4N/internal-runtime-stage2-check/moved path/gb-monitor/`，约120MiB。先原路径，再整体搬到含空格路径并从任意工作目录启动；SHA256SUMS、核心库包内加载、Qt TLS实际初始化、包内OpenSSL、程序/Qt插件 `ldd -r` 及相对RUNPATH通过。检查工具的2秒启动使用有归属SIGTERM，不冒充正常GUI退出；另由 `package-gui-stage2-01/` 通过XTest真正运行包内ELF，持续变化画面10.748秒、默认静音→实际PCM波形→静音、WM_DELETE退出0，所有本轮进程回收。此包测试只验声音状态，不用于报告开关时延；首次冷开声尚无剔除录制链路延迟的测量。监督结果中的退出码255属于本轮主动SIGTERM结束的FFmpeg publisher，并非GUI异常退出。

- manifest SHA256：`7998fedb851214c1397a98e587e881ea5d217d50b9bc6953156b7b1c61b1e730`。
- 已安装ELF SHA256：`5622ce14ac167ea40e2b8663fa9088487b0b2e3bed5f4c12fd67dd58192f7e3f`；与构建目录ELF不同源于CMake安装时替换RUNPATH，不将两个哈希混用。
- manifest如实记录提交前HEAD及dirty，不宣称clean提交发行包。先一次打包因输出父目录不存在失败，保留 `package-build.log`；显式创建本轮空父目录后成功，未修改打包器或掩盖失败。
- 仍为内部验证：项目许可证及自定义GCC runtime完整许可来源未闭合，公开分发BLOCKED。宿主glibc/加载器、字体、驱动及音频服务等边界保持阶段一约定；无第二机器验证。

## 原始失败及后续边界

同GB通道四格的首轮 `gb-monitor-quad-cold-01` 保留FAIL：10秒每路235帧、451200 PCM样本，均为标称94.0%，未达到既有95%阈值，因而没有进入停前三格的后续阶段。该轮实际编排遗漏已验证的 `--phase-buckets 1`，使用模拟器默认40阶段、1ms ticker；两轨墙钟进展同样偏慢，无播放错误或解码丢帧。仅修正本轮测试编排，不修改模拟器、客户端或健康阈值；修正后的结果另行记录，不能把本轮失败改成PASS。

实际两音源切换首轮也保留FAIL：脚本把设备列表顺序当作固定顺序，选中1000Hz源却等待440Hz，未进入50次切换。第二轮50次异频切换已经通过，但后续同频资源检查的ready窗口混入点击前旧PCM，使后续稳态断言失败；第三轮只修正测试中的设备ID到可见行映射及完整100ms FFT窗口必须来自点击后，不放宽频谱隔离或时延阈值。原始脚本、PCM和失败结果均保留。

Claude Code第12～14轮只读复审未发现可达P0/P1，最后明确可提交开发分支；正常/sanitizer/搬移及文档门槛已实际执行。保留QObject context、异步线程保活和单设备借用，不机械提取Catalog helper或新增关闭状态。一个P2使用边界：静音时设备被移除，首次开声会发现失效并提示，下一次用户手动开声才重新打开，不自动重试；尚未把硬件热拔插作为真实验收。

阶段二观看门槛完成，可以提交并继续阶段三。公开分发许可、第二机器、物理设备PTZ动作、录像/下载/告警/对讲仍属于明确后续边界，不写PASS，不据此宣布完整产品GO。原始证据、失败记录、最终运行包、匹配shared SDK和Claude讨论保留。

阶段末仅清理本轮两个可再生Python字节码及搬移后留下的空父目录；私有Pulse按PID/starttick/exe/namespace/专用环境逐项核对后SIGTERM，socket消失，桌面Pulse原starttick不变。未清理原始失败、测试脚本、媒体、最终包、构建缓存或其他Agent资源；保留 `pulse/cleanup.json`。
