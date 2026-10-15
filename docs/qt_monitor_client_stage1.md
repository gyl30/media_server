# Qt 客户端阶段一验证记录

本记录只针对单路观看基础，不代表完整 GB28181 平台或 16 路验收。开发基线为 `ba803c2a8c0d2a82298dd9a9f68404cc0f57f320`，独立分支 `feature/qt-monitor-client`；不改变服务器 C++ 架构、不合并 main。

## 实现范围与边界

- 传统 Qt Widgets 监控外壳、平台连接、设备/通道/普通源树和单画面；未实现的业务入口明确禁用。
- 控制使用既有 signaling API；每次播放独立一次性票据，FFmpeg 以 RTSP/TCP 接收 H.264，音频 AAC/PCMA/PCMU 解码并重采样至 48kHz/stereo/S16。合法无音频正常播放；未支持的已声明音轨明确拒绝。
- 媒体缓冲有界；独立解码线程可中断，GUI 不等待网络 join；QObject context 隔离旧请求及迟到回调。
- GB 冷点播原先会在媒体尚未发布时提前访问 RTSP。现在只从既有对账缓存派生 `live.media_ready`，不新增状态、数据库字段或 endpoint；客户端在同一票据/同一 live 代际内最多等待 15 秒，不重新签票。缓存约五秒刷新，不承诺即时就绪。
- 切换平台地址会取消原平台待播放请求；同平台刷新只更新树，不结束观看或再次认领票据。
- 默认静音但持续解码音轨。SDL 进程级初始化；首次开声才打开画面设备，静音 Pause/Clear，停止/源结束/错误 Close。没有新增音频管理器或重连状态。

## 环境与插桩

开发机 Linux x86_64，GCC **16.0.1 experimental**，Qt 6.2.4，SDL 2.0.20；正常 RelWithDebInfo 为 `-O2 -g -DNDEBUG` 并保留 `-Werror`。桌面是独立 CMake 工程，不依赖 Boost.Context。

另行从本机 FFmpeg 9.0.1 源码构建最小 shared decoder SDK，实际 license 函数为 LGPL 2.1-or-later，不启 GPL/version3/nonfree，原服务器静态 SDK 不动。ASan+UBSan 编译第一方桌面代码，使用 `-O1 -g -DNDEBUG -fsanitize=address,undefined -fno-omit-frame-pointer`。Qt、SDL、FFmpeg 及系统库并未完整插桩；不能把第一方 PASS 扩大为这些依赖全部内存安全证明。未禁用 LeakSanitizer、未设置 suppression。

## 自动测试

| 验证 | 实际结果 | 原始证据（DEV） |
| --- | --- | --- |
| signaling 全量 test / race / vet | PASS | `signaling-ready-full-test.log`、`signaling-ready-race.log`、`media-ready-vet.log` |
| 正常桌面 CTest | 4/4 PASS，39.01 秒；QTest 41 项、0 fail/skip（含 init/cleanup） | `review-final-ctest.log`、构建目录 `Testing/Temporary/LastTest.log` |
| ASan+UBSan 桌面 CTest | 4/4 PASS，39.14 秒；无 sanitizer 诊断 | `review-final-asan-ctest.log` |
| 切换平台取消旧 GB 等待 | 先 RED，再 GREEN；同平台刷新继续等待，换平台后无旧通道请求串线 | `platform-switch-red.log` 与上述 GREEN |
| 无效平台输入不修改当前配置 | 先 RED，再 GREEN；原平台就绪查询继续，无重签 | `review-p2-invalid-url-red.log` 与上述 GREEN |
| 请求及播放生命周期 | 超时、取消、替换、销毁、GB 就绪失败/代际变化/15秒总期限 | `platform_client_test`、`playback_view_test`、`monitor_window_test` |
| 解码器 | AAC/PCMA/无音轨、Opus拒绝、立即取消、真实RTSP挂起取消/15秒超时 | `decode_thread_test`，fixture CLI `/home/gyl/bin/ffmpeg` |

除 GB 专用目录外，下文相对证据路径均在 `/var/tmp/media-server-qt-stage1-bFmYuf/`。原始文件含私有授权，仅保留在机器上，不提交 Git。

## 真实媒体与声音

普通源经真实 FFmpeg 发布、media_server、signaling 一次性授权、Qt FFmpeg 解码及 QWidget 交付，均非 mock HTTP/RTP。测试素材为 640×360、H.264 Baseline、25fps、GOP25、无 B 帧；AAC 48kHz/stereo 或 PCMA 8kHz/mono。

| 场景 | 结果 | 证据 |
| --- | --- | --- |
| shared SDK AAC，有声 20秒 | 110项 PASS，0 fail | `final-aac-markers/` |
| shared SDK PCMA，有声 20秒 | 110项 PASS，0 fail | `final-pcma-markers/` |
| 合法无音轨 10秒 | 39项 PASS，0 fail | `final-video-only/` |
| 反复声音切换 16秒 | 98项 PASS；观测视频99.75%、PCM99.87%，每秒交付画面均增长 | `real-aac-pause-toggle/` |
| ASan AAC切换后静音停止 15秒 | 92项 PASS，0 fail，无 sanitizer 诊断 | `asan-aac-toggle-muted-stop/` |
| ASan PCMA有声 10秒 | 60项 PASS，0 fail，无 sanitizer 诊断 | `asan-pcma/` |
| 模拟 GB 冷点播 PCMA | 无预热、单票据、INVITE/ACK各1，观看退出不停止共享live；显式停止后BYE及资源回收 | `/tmp/qt-gb-plan-WDjxT3VH/cold-ready-01/` |
| shared ASan 模拟 GB 冷点播 | 50项 PASS；10秒250帧/480000 PCM样本，关闭100ms且对象析构；四服务回收 | `/tmp/qt-gb-plan-WDjxT3VH/cold-ready-shared-asan-phase1/` |

探针共同窗口要求视频实际解码且图像交给 view、已声明音轨实际解码后才开始；逐秒检查各轨 PTS/计数，窗口内视频及 PCM 至少达到标称95%，交给 view 的图像至少为解码量95%。`rendered_frames` 不是硬件垂直同步/扫描计数；声音状态和 PCM 解码量也不能替代真实音频输出检查。

### 音画数字回环

使用本轮私有 Xvfb 和私有 Pulse null sink，同一 FFmpeg 录制文件保留屏幕与音频 PTS；先完成两秒真实采样预热，再启动播放。视频中心白闪和同步哔声每两秒一次，10ms RMS定位音频，50fps定位图像。

- 最终 AAC：11个匹配标记（含首个），A−V中位数 **−12.500ms**，范围 **−20.500～+0.500ms**。
- 最终 PCMA：10个匹配标记（含首个），中位数 **−12.021ms**，范围 **−19.521～−9.521ms**。
- 先前10分钟 AAC：300个标记；首个 +129.979ms 独立保留为旧起播问题，其余299个 −28.521～+11.479ms；首/末分钟中位数 −9.521/−10.521ms，拟合 −0.406ms/min。该轮在后续起播/切换修复前，不可笼统继承为最终代码的完整十分钟 PASS。
- 后续让有声播放先建立音频时钟，并改善采样预热后，最终首标记及稳态通过；不能把两个同时变动的效果只归因于其中一个。

这是数字回环，不是物理扬声器/麦克风声学测量，时间量化和录制链误差已保留，不宣称精密硬件同步。

## 原始失败与修复边界

1. RTSP 初始探测历史缓存导致突发挤掉未来视频帧：启用 FFmpeg `AVFMT_FLAG_NOBUFFER`，继续等自然关键帧，不引入 GOP replay。
2. Qt 本机 GStreamer 音频后端的队列/处理计数不能提供所需播放时钟，改用 SDL2 有界 PCM 队列；时钟仍是含后端双 period 的估算。
3. 反复关闭/重开 SDL 设备后，独立实验复现约一秒 PLAYING 但队列不消费，随后恢复；旧 trace 不能称永久冻结。静音保留同会话设备，停止才 Close，严格切换测试 GREEN。没有改 Pulse 或系统音频配置。
4. 默认模拟器40个phase用1ms ticker累积推进帧，单设备 source 时钟受调度影响。首轮 shared-ASan 冷 GB 视频与PCM均94.8%，健康断言真实 FAIL；无内存报告，未降低95%标准。仅在临时脚本为单设备设置 `--phase-buckets 1` 后，两轨实际100%；同live被动抓包末段约24.992fps、零丢包。旧失败原样保留，原轮没有上游抓包，不能断言其具体漏tick数。

## 交付限制

内部包已实际搬移至 `internal-runtime-stage1-final-check/moved path/gb-monitor`；全量 SHA、`ldd -r`、相对 RUNPATH、Qt/FFmpeg/SDL 包内加载及 Qt OpenSSL 实际包内加载通过。manifest SHA256 为 `4aa36cfe6f4a46011013314643a456c8ba87f5a08e57b7a95fe61af3f7822013`，明确记录当前提交前 dirty 构建与第一方源码 SHA，不冒充纯净 HEAD 发行物。打包 CLI 七项负对照通过，已有输出不覆盖、错误 SDK/插件/版本明确失败；原始依赖冲突与 TLS dlopen 逃逸失败均保留。

Claude Code 同一会话第10/11轮只读复审未发现阶段一 P0/P1；其提出的无效输入、最后1ms期限和 shared 链接清单问题已最小调整并复测。复审不是独立运行测试，阶段提交仍以真实执行结果为依据。

搬移后的包内真实 `gb_monitor` 已由私有 Xvfb 与 XTest 实际点击设备及声音按钮验证，未用探针 ELF 替代。`package-gui-final-04/acceptance-report.md`、`playback-result.json`、24张原图及私有 monitor PCM 保留：持续观察10.846秒、各截图画面持续变化，默认静音 RMS=0、开声后 RMS=667.63/peak=2899、再次静音 RMS=0；`WM_DELETE_WINDOW` 正常退出0，本轮服务和 Xvfb 已回收。音频观察含 monitor 缓冲与轮询误差，不作为精确声卡首响延迟；GUI截图也不冒充95%帧率计数验收。实际 `/proc/PID/exe` 与包内 ELF SHA256 `c57e97ff788d891041aff129f54fe7892d4a25864010a7885ad9377bbecfeb68` 一致。库加载闭包由独立包校验记录证明，GUI wrapper 对带空格 maps 路径的摘要并不完整，不能单凭它证明依赖全部来自包内。

阶段一门槛完成，可提交开发分支并继续第二阶段；这不是完整产品 GO。没有对外分发许可闭合；没有真实摄像机物理动作、第二机器、16路、PTZ/录像/下载/告警/对讲验收。后续阶段按 [实施计划](qt_monitor_client_plan.md) 继续。
