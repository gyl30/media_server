# 第二机实际播放与数字音频补验

## 范围与版本

2026-10-17（UTC+8），在 TEST `172.20.63.32` 实际运行 Qt 播放进程，经生产 Go 实时点播、SIP 与 C++ 媒体服务，观看大华 `172.20.44.240` 的 H.264 Main 1920×1080/25fps、PCMA 8kHz 单声道。单路及同源四路通过，不代表四台设备、第二机16路或正式容量。

基线 `347decb6bc4dd9bad03080da4bc4fbd1b7a715bf`；Qt 源码与已完成广播 SDK 前置回归的版本相同，已实际检查 `git diff 5efffbf HEAD -- desktop tests/platform_client_test.cpp` 为空。本轮正常增量构建通过，未重跑桌面 CTest、Go 全量或 ASan/UBSan，不继承历史测试为本轮执行。

Qt 探针链接真实 `monitor_window`，通过既有树和按钮信号驱动播放、声音及停止。它不是安装版 `gb_monitor` ELF 的外部鼠标验收。Go 为私有 test overlay 启动生产服务，口令从指定0600文件读取；媒体服务器复用既有冻结 ELF。没有模拟设备、假在线、修改相机配置或注册规则。

媒体 ELF SHA `9abe7ac69495ed25a8946182db9bbfa6671edd15fe4f809da151945c48526fe2`，来源为既有 `pause-integration/elf/media_server-normal`，在a2fcc80时打包；其精确构建提交未在包manifest记录，不称为347decb新编译产物。已核对 `git diff a2fcc80 HEAD -- src include CMakeLists.txt tests` 为空，只证明这些媒体源码路径未变。Go overlay ELF SHA `cb5cb60e59c8c88d3b2486346c6184b0ca57f6d3e03e2fd8b54582c1cb1c1358`，源码保存在本轮证据根。

预声明“观察至65分钟”实际执行为首次注册/目录的最长等待预算，满足真实 `getOnline` 和通道ON条件即继续，未固定运行65分钟；原预声明文字保留，按此澄清解释边界。`platform-ready.json`记录条件成立，于07:55:42.706589（UTC+8，服务启动后45.870秒）落盘；这不是REGISTER报文的精确到达时刻，本轮没有独立注册抓包。整个服务观察约352秒。

## 实际结果

| 项目 | 单路 | 同源四路 |
| --- | --- | --- |
| 共同播放观察 | 12秒 | 30秒 |
| 首帧 | 5405ms | 各2845ms |
| 原探针断言 | 75通过，0失败 | 574通过，0失败 |
| 播放进程 | 2202020，退出0 | 2202196，退出0 |
| 声音操作 | 第一路开启 | 第一路每2秒切换，15次 |
| 停止 | 99ms，解码对象回收 | 停三留一后再观察3秒，剩余路呈现741→816帧；最终100ms停止 |
| Pulse数字采集 | 779509个双声道采样帧，峰62 | 1706746个双声道采样帧，峰64 |

原探针的95%视频解码速率、呈现比例及重采样音频速率断言未改；所有路的双轨和画面持续增长。四路停止其余后，3个解码对象销毁且剩余路继续工作。

首帧数值来自探针轮询观察，非逐帧精密时延。单路最终统计丢弃视频5、音频1帧，四路视频5–6、音频各1帧；仍满足原门槛。四路均独立RTSP/解码并共享一个实时stream_id，不外推为四个设备上游。

TEST 独立 PulseAudio null sink 以48kHz、s16le双声道采集；`sink-inputs.json` 中 `application.process.id` 与上述 Qt PID 一致。非零样本分别1210516、1496578，PCM SHA分别为 `d933888159a9de7c29129c613a1b1ddc47e6f24e4b28dff6f5af371b9f040610`、`68a2f053c7ddc926321bb5081285260192b0f9d527c328ac32256408f83951fb`。采集电平低，RMS约10.66/7.65；只证明归属于Qt的非零数字音频交付，未做内容标记关联，不验证物理扬声器、主观听感或音画同步偏差。

窗口使用 TEST XCB，经认证 SSH loopback6198 转发到 DEV 本轮独立 Xvfb UNIX socket；Xvfb 禁止TCP监听，使用独立认证文件。未使用用户桌面或修改 `.Xauthority`。6张屏幕采集保留，主代理人工查看 `screen-03-play-4.png`，确认四格实际相机画面；TEST Qt/FFmpeg/SDL/XCB 的进程映射均核对为本轮包内依赖。显示服务器位于DEV，因此不是TEST物理显示器验收。

## 运行包与证据

包 manifest 标记上述 clean HEAD、`git_dirty=false`，使用独立 FFmpeg 9.0.1 LGPL SDK；包内依赖与SHA检查通过。归档SHA `85fe8068d848019c5bfebe9a7a82508a00e00f134c0aa19d3b0afb90041e2ab6`，manifest SHA `9f5e0dd01310f4a25ca730bce91c9f05bfbbe32107c2bccdf929119b8f596c0f`，播放探针 SHA `3af51dfba6defa56a265327f540f10835c16deea882e34f6d50510d49919d2e7`。探针是独立验收资产，不混称安装版程序。

私有证据根：DEV `/var/tmp/qt-second-play-347decb-uu6iwq6l`；TEST `/var/tmp/qt-second-play-347decb-kqUvCVLZ`，实际服务与播放位于 `run-02`。包含预声明方法、私有启动源码、构建日志、精确ELF、运行包、原始PCM、探针JSON、截图、依赖映射和清理记录，不随Git公开复制。

原始工具失败保留：首次媒体启动缺少私有库路径，未进入播放；修正本轮监督器后另建 `run-02`。两组播放完成后，外层 `scp -r` 遇活的 Pulse UNIX socket，`gui-run-01/result.json` 原样保持 `INCOMPLETE`，其中 `remote_exit=0`。只补收普通文件并逐项核验SHA，不重写原结果或无故重跑设备播放。TCP关闭后的TIME_WAIT曾阻断临时bind检查，保留失败日志，不当作服务仍存活或直接跳过检查。

本轮服务按 Go排空→媒体→Pulse 顺序退出，均退出0、PID消失，平台 `live_sessions=0`、`broadcasts=0`。Qt、私有Xvfb与SSH隧道均回收，两机临时X认证及本轮Pulse cookie已删除；`display-cleanup-verified.json`补核实Xvfb PID消失、无本轮精确转发命令、TEST6198可bind及两机认证文件不存在。服务端38520–38523/TCP、15060及49152–49153/UDP均可重新bind，29项普通文件回传SHA全部一致。

回传归档SHA `f53308d0de5ca8127eea460a1eecd7a1ca31ae137a6c3ce1de40160b2d6da75e`，清单SHA `7ddf7194f3dbfbc4030dfc0e5d8e6e2efe5d19836e8eed3d8abe9de8db8dc55b`。这是传输完整性核对；临时cookie删除及清理记录是在运行后进行和补写的，不把它当成全程不可变证明。服务清单的 `STOP_REQUESTED` 是监督器退出原因，退出码/PID和独立清理记录分别保留。

## 未闭合边界

本项补齐第二机实际单路/四路 GB 播放、虚拟屏幕和数字音频；物理屏幕、物理声音、麦克风、输入法、第二机16路及包内安装版外部播放操作仍未验收。公共分发继续 `BLOCKED`，GCC运行库匹配源码等许可义务未闭合；不按安装目录名称推断编译器版本。

广播旧实验仍 `INCOMPLETE/INCONCLUSIVE`，音量恢复97的既有事实不变；本轮没有发送广播。广播公开API、Qt采集/UI、实际设备声音，同会话seek/倍速等仍需接续开发，不能给出完整平台GO。
