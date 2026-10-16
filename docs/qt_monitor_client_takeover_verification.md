# Codex 接手与阶段七部分验收

## 范围与版本

2026-10-16 按用户要求停止原 Claude 会话及其额度恢复自动续接，由 Codex 接手。只终止已确认属于本任务的 launcher 2901253 和等待进程 3025161，保留原日志与其他 Agent；停止记录位于 DEV `/var/tmp/qt-claude-resume-EIl5GIZJ/status.txt`。

唯一开发工作区 `/home/gyl/example/media_server_qt`，分支 `feature/qt-monitor-client`。本轮接手 HEAD `59bd0cbc66a15527bb1912e0f7a1628d434cb89f`；报警生产与测试提交 `a2da6dd770839c721282e711262763489fe38447`。main/origin/main 在本轮检查时均为 `ba803c2a8c0d2a82298dd9a9f68404cc0f57f320`，未修改或合并 main、未改 third。该分支正常推送，不改写既有提交。

保留阶段一至五已实现的观看、查询、PTZ/预置位、目录、有限回放与暂停、本地录制与下载；未重复实现。历史结论仅在各自阶段文档范围内成立，本轮不把历史测试当新执行。

本轮原始证据根：DEV `/var/tmp/qt-codex-resume-tRclw0/`。日志可能含授权，不提交 Git。下面列出的文件已实际检查存在；证据目录不是随仓库分发的资产。

## 报警增量与完整桌面回归

修正重启后序号追平旧游标造成漏报，以及切平台后旧请求没有取消；完整协议、所有权和 RED/GREEN 记录见 [阶段六报警](qt_monitor_client_stage6_alarms.md)。由独立代理进行只读代码复审和实际协议组合验证，没有要求已停止的 Claude 继续开发。

| 项目 | 结果 | 实际范围与证据 |
| --- | --- | --- |
| 桌面 Normal | PASS | 完整构建；7/7 CTest、396 项 QTest，0 失败/跳过，229.00 秒；`qt-final-build-normal.log`、`qt-final-ctest-normal.log`、`qt-normal-LastTest.log` |
| 桌面 ASan+UBSan | PASS | 完整构建；7/7、396 项，228.41 秒；`qt-final-build-asan.log`、`qt-final-ctest-asan.log`、`qt-asan-LastTest.log` |
| Go test | PASS | `go test -count=1 ./...`，主模块 116.163 秒；`go-full.log` |
| Go race | PASS | `go test -race -count=1 ./...`，主模块 122.617 秒；`go-race.log` |
| Go vet | PASS | `go vet ./...`；`go-vet.log` |
| 报警实际组合 Normal/ASan | PASS | 每组实际 Qt→生产 HTTP handler→UDP SIP peer，3 次通知与独立响应，切平台/取消/隐藏/析构均核验；11.214/11.680 秒；`alarm-integration/verification.md` |
| 媒体服务器完整 CTest | NOT_RUN | 本轮未修改媒体服务器 C++ 生产实现，没有重跑服务器旧 69 项，不混用桌面 7 项与服务器数量 |

编译器实际 GCC 16.0.1 experimental，Qt 6.2.4、SDL 2.0.20、Go 1.27.2。桌面使用独立 shared LGPL FFmpeg SDK `/var/tmp/qt-claude-owner-work/ffmpeg-mkv/install`；正常与 ASan 构建位于 `/var/tmp/qt-claude-owner-work/desktop-build/{normal,asan}`。RelWithDebInfo 实际 `-O2 -g -DNDEBUG -Werror`，ASan 另含 `-fsanitize=address,undefined -fno-omit-frame-pointer`。泄漏检测开启，无 suppression，无 sanitizer 诊断。Qt/SDL/FFmpeg 依赖并非全部插桩，不能扩展为所有依赖的完整内存安全结论。

## 最新版本 16 路观看

证据 `stage7-monitor16-01/result.json`、`playback-result.json`、截图及资源采样。首轮 PASS，2009 项断言、0 失败，探针退出 0。使用实际 `monitor_window`，不是仅建立连接或显示空布局。

- DEV i7-13700KF；同源 16 个独立票据、RTSP 连接及 FFmpeg CPU 解码实例。
- 固定 H.264 Main 1920×1080、25fps、约 4Mbps，AAC 48kHz 双声道；20 秒素材循环。
- 16 路首帧 2696ms；共同观察窗口 2796–32796ms，共 30 秒。
- 每路窗口内 750 视频解码帧、750 交付 view 帧、1405 AAC 帧、1,438,720 PCM 样本；各路双轨逐秒持续进展，队列丢弃为 0。
- 停止耗时 99ms，16 个解码线程全部 finished/destroyed；媒体、信令、Xvfb、probe 退出 0。发布者由监督器主动 SIGTERM 后退出 255，不是未解释的媒体失败。5 个本轮子进程全部回收，38100–38104 最终 TCP bind 检查空闲。
- 33 次资源采样、跨度 32.17699 秒，包含 ready 阶段：平均 2.14843 CPU 核，峰 RSS 301.5586MiB/PSS 250.7178MiB、FD 51、线程 20。这不是隔离稳态性能基线，不据此推导容量。

独立 Xvfb/XCB；静音但所有音轨真实解码，SDL dummy 不代表物理声音。交付 view 不等于显示器垂直同步；此项不是 16 个不同源、第二机 16 路或物理摄像机验收。

| 资产 | SHA256 |
| --- | --- |
| 素材 `h264-1080p25-aac.mkv` | `717951f2a7e2c49eab30f44a4a669d3c80318bc52a134dfb7f3f12eb64acfcb9` |
| 冻结媒体服务器 ELF | `9abe7ac69495ed25a8946182db9bbfa6671edd15fe4f809da151945c48526fe2` |
| 本轮 signaling ELF | `d0e3d159b8e33b266f5c95864e1e51736cca2c105f76b3df250cb010ede45bba` |
| 本轮 Qt probe ELF | `9737384f2bc7aef71b93c525e028263b8f0d8bf16fd588bcd6109fed88ebbdb9` |

媒体服务器使用此前冻结 ELF；信令构建发生在报警提交前，源码对应本次报警变更，不能将它称为提交后 clean 构建；运行包的 clean 版本另见下节。probe 使用本轮更新后的桌面源码，未修改健康断言。

## DEV 内部包与重定位

`stage7-package-04/result.json` PASS。复用既有 `desktop/package_internal_runtime.py`，manifest 准确标记源码 HEAD `a2da6dd770839c721282e711262763489fe38447`、dirty=false。后续仅文档提交不会改变该已封存包的来源身份。

已验证生成、SHA 清单、整体搬移到含空格路径、从 `/` 启动 `run.sh`、实际 1280×800 Qt/XCB 窗口、Qt/FFmpeg/XCB 私有库映射及 WM_DELETE 正常退出 0。未修改主开发服务、系统库或原 SDK。

| 资产 | SHA256 |
| --- | --- |
| `stage7-runtime-a2da6dd.tar.gz`（约 49MiB） | `d326ad69af1111a70ccaf737be84f6024d6b9867b45c71bc8d3ccf309f187be1` |
| package-04 manifest | `400cac1fa9bab4f2b71a01820dcdd6c0ebd8e57a4a819518caef6f3c197dc218` |
| build `gb_monitor` | `b4fa8c03868d75522ac562966bf1fde7eb3434a2f030a074e94f531aba22fb80` |
| installed `gb_monitor` | `a81d4b26756fdc38fe45074047e3f463f2a84bacf49beda18f328880bf9e2dd0` |

安装时 CMake 改写 RUNPATH，因此 build 与 installed ELF SHA 不同；不是静默换版本。

两次私有监督器失败保留：package-01 通过 XFetchName 匹配中文标题失败，实际 WM_NAME 为空；package-02 将含空格 maps 路径截断，误报包外库。只修正本轮监督器的窗口识别与路径解析，未改生产或打包器。package-03 首次通过时准确记录 `59bd0cb+dirty`，没有伪称 clean；随后另建 package-04，未覆盖失败或旧包。

## 第二机基础验证与边界

经 DEV SSH/SCP 传到 TEST `172.20.63.32` 的全新 `/var/tmp/media-server-qt-stage7-clean-Yp9aS9/`。目标为 Ubuntu 22.04.5 x86_64、glibc 2.35、Xeon Silver 4216；检查时约 79GiB 可用 RAM、54GiB 空闲磁盘、swap 0。

- 归档 SHA 与 DEV 完全一致；内部文件清单、程序和 offscreen 插件 `ldd -r` PASS。
- 实际 offscreen 运行，Qt Widgets、SDL、FFmpeg 和平台插件全部从包内加载 PASS；见 TEST `offscreen-result.json`，DEV 镜像为 `test-clean-offscreen-result.json`。
- 初轮 offscreen 的正常 GUI 退出 NOT_RUN；生产程序无外部 offscreen 正常关闭接口。仅对本轮 PID 2182074 发 SIGTERM，退出 -15 并回收，核对 `/proc` 消失；不把该结果写成正常关闭 PASS。
- 初轮第二机隔离 XCB GUI BLOCKED：无 Xvfb，不使用用户物理 X0，不安装软件或改变全局配置。后续以独立 SSH/Xvfb 方式补验，见下节，原 offscreen 结果未改写。
- 第二机实际媒体播放、16 路、物理音频、输入法 NOT_RUN。基础库加载通过不等于这些项目通过。
- 另一 Claude（PID 2153042、cwd `/root/svac_rea`）及旧 namespace 保持原样，没有全局清理或停止他人服务。

旧 TEST 目录 `/var/tmp/media-server-qt-stage7-CwoqK9/` 保留，其 dirty manifest 不能与 clean 包混淆。当前包仅为内部搬运验证，`PUBLIC_DISTRIBUTION=BLOCKED`；许可与匹配源码等分发义务未闭合，不宣布公开发行。

### 第二机 Qt/XCB 与正常关闭补验

后续 `stage7-test-xcb-01/{result.json,test-result.json}` 首轮 PASS，TEST 证据为 `/var/tmp/media-server-qt-stage7-xcb-6RSg4c/`。实际运行的是 TEST clean 包中的安装 ELF，SHA 为上述 `a81d4b...9e2dd0`，并非在 DEV 运行同名程序。

TEST 实际创建 1280×800 XCB 窗口；进程 maps 核验 Qt、FFmpeg、SDL 与 XCB 插件全部来自 clean 包。DEV 向本轮私有窗口发送 WM_DELETE，TEST 应用退出 0，PID 2182921 已 reap 且 `/proc` 不存在。此项确认第二机窗口及正常关闭 PASS，不替代第二机媒体/声音/输入法测试。

显示服务器位于 DEV 本轮独立 Xvfb，由 `-displayfd -nolisten tcp -auth` 分配空闲显示并认证；SSH 仅在 TEST loopback 转发 6197 到该 Xvfb UNIX socket。没有 `-ac`、没有 DEV TCP X11 暴露、没有修改用户 `.Xauthority` 或使用物理桌面。自动分配显示名恰为 `:0`，但 PID 3319011 与命令身份确认它是本轮新建的 Xvfb，不是用户桌面。隧道、Xvfb 及应用均回收，6197 可重新 bind，两机临时 cookie 删除，日志与结果保留。

## 未完成工作与结论

当前增量具备代码审查条件，完整七阶段平台仍未完成，不能给出整体 GO：

1. 同会话 seek/回放倍速未实现。需要依据有效 RTP-Info 识别新位置，明确跨有限通道传递边界并刷新 Qt 解复用、解码、PCM 和呈现时钟；重开会话或猜 PTS 不能冒充 seek。
2. 广播/对讲未实现，不只是缺物理验收。现有 PS sender 要求视频，Qt 当前只有播放音频；需窄实现采集、协商、纯音频媒体方向与 owner 清理。用户尚未指定可测试的麦克风/扬声器设备与授权，不能用 SIP 200 当设备端音频成功。
3. 真实云台/预置位、物理 NVR 回放下载、物理报警触发仍受设备条件限制；受控 peer 结果仅覆盖实际执行的协议链路。
4. 第二机本地桌面/声音/实际播放和公开分发边界仍须单独完成；SSH 私有显示下的 XCB 窗口与正常关闭已通过，不能扩大为这些项目通过。

本轮自有进程已回收，保留原始失败、日志、运行包和复现命令，不删除他人工作区或历史证据。已停止 Claude 自动续接，后续由 Codex 继续开发，不存在另一个自动后台开发者。

## 后续开发的只读审查结论

以下是尚未实施的设计约束，不是已支持功能或测试 PASS：

- 广播先限定单目标、UDP、PCMA；复用现有 G.711 RTP 打包与有界 UDP transport，不把视频 PS sender 改成音频 sender，也不新建通用输入工厂。Qt 当前 SDL 只播放、私有 SDK 未启用 `pcm_alaw` encoder，采集与编码链路确实待实现。
- GB/T 28181—2016 §9.12 的广播有业务 Response 和设备发起的反向音频 INVITE；不能直接套用现有平台发起的视频 UAC。需要最小 UAS owner、ACK/CANCEL/BYE 与迟到回调测试，不能把 SIP 200 当广播已完成。对讲可组合既有实时音频“听”与广播“说”，不必为了名称另建一套纯音频接收器。
- seek 需先准备接收端边界，再发 INFO、校验内层 CSeq/Range/RTP-Info，并在新位置媒体前交付明确边界。RTP-Info 的 rtptime 是时间映射，不保证等于首包 timestamp；seek 也不意味着可以取消连续 sequence 校验。原始 PS 不含该控制边界；HTTP chunk extension 可能被代理移除或重分块，不能默认为任意网关下可靠的协议。
- 倍速不能复用 Download 的 speed 参数，亦不能未经实际 PTS/音频样本验证就给 Qt 墙钟乘倍率；设备可能已经按 Scale 改变媒体时间轴。需先获得真实设备或受控协议栈的可核对行为。

标准证据保留于本轮 `GBT28181-2016.pdf`/文本摘录；依赖核对基于当前锁定的 sipgo 和 RTP 库，不假定上游最新实现。下一阶段应先落实一个窄契约并定向验证，不同时扩大 seek、音频 UAS、采集及通用媒体结构。
