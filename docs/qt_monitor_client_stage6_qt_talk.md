# 阶段六：Qt 麦克风上传

## 实现

接续[Go 对讲控制](qt_monitor_client_stage6_talk_control.md)，Qt 复用 SDL、FFmpeg 和 Qt WebSockets，不实现另一套 SIP、RTP 或 WebSocket。选中父国标设备或其通道后，“开始对讲”绑定该父设备；回传声音仍由相应实时画面的“开启声音”独立控制，不自动打开扬声器或承诺已听见设备。

新增 `talk_session` 是一次采集/上传 owner，不是通用输入工厂。它固定创建时的平台地址和设备身份：先打开默认麦克风并准备 PCMA 编码 → POST 对讲 → 校验一次性上传票据 → WebSocket 连接成功后才解除采集暂停。平台切换不改变旧 owner 的 DELETE 目标。

- SDL 请求并核对 8000Hz、单声道、native S16。callback 长度不固定，只写有界 3200 字节 PCM 缓冲，满时按完整样本丢弃最旧内容，避免 GUI 卡顿形成无限积压。
- GUI 精确定时器每 20ms 最多取 320 字节、编码 160 样本、上传一个 160 字节二进制消息；不补发错过的 timer 次数。沿用原 SDK 已启用的 FFmpeg PCM_ALAW encoder。
- callback 和 GUI 间只有保护 PCM/采集进展时钟的 mutex；不增加线程、通用队列或关闭状态。SDL_CloseAudioDevice 在锁外等待 callback 完成，随后清理缓冲和编码器。
- WebSocket 握手 5 秒、真实有效采集进展 3 秒期限；静音也是有效采集。Qt 待写字节超过 1660 时终止，不增加无界网络缓存。该限制不是内核发送队列或端到端实时性的保证。
- AVPacket 在发送前复制到独立 QByteArray 并 unref；WS 同步错误若重入 stop，返回后根据采集设备资源状态退出，不再访问已释放编码器。
- stop 先断 WS 到 owner 的连接、abort 上传、关闭采集，再精确 DELETE 本次 ID。启动中停止保留 owner 到 POST 有界结束，迟到 201 仍清理；关闭窗口沿既有异步收尾模式等待，不阻塞 GUI 等网络，不用额外 closing 标志。
- 成功响应完全丢失时可能拿不到 ID，仍受 Go 请求取消和媒体 20 秒期限约束，不 GET 后盲删其他会话。POST 15 秒、DELETE 5 秒，不自动重试。

Qt 网络 TLS 错误不会忽略。本增量完成时 Go 只发布 LAN `ws` URL；后续[Web采集与可信HTTPS/WSS](qt_monitor_client_stage6_web_talk.md)补充公开媒体origin，历史Qt测试结果不因此扩大为真实WSS或物理听音验收。

## 构建依赖

Qt WebSockets 是新增的标准模块，必须匹配 Qt Core/Network；不改媒体服务器依赖。DEV 实际为系统 Qt 6.2.4，缺少 WebSockets 模块，因此只下载并在本轮独立前缀提取同发行版的 `libqt6websockets6`、`libqt6websockets6-dev` 6.2.4-1，没有 sudo、系统安装或 Qt 升级。

私有前缀：`/var/tmp/qt-talk-capture-5c6ec00-jqnb72vj/qt-websockets/usr/`。构建以 `Qt6WebSockets_DIR` 明确指向其中的 CMake 包；正常部署应提供匹配的开发模块，或使用已核对的独立前缀。不在源码中硬编码这个路径。

保留两个 deb 的 SHA、版权原件与下载记录。其模块版权声明为 LGPL-3/GPL-2 选择；内部包包含匹配的私有动态库，不提交预编译文件。现有打包器无法通过系统 dpkg 数据库识别未安装模块的版权来源，manifest 如实将其列入 unresolved；项目整体公开分发许可仍 BLOCKED，不能把搬移运行 PASS 扩大为可公开发布。

## 实际验证

证据根：DEV `/var/tmp/qt-talk-capture-5c6ec00-jqnb72vj/`。原始网络/授权材料不提交 Git。

| 验证 | 实际结果与范围 |
| --- | --- |
| Normal 完整桌面 CTest | 9/9 PASS，184.23秒；完整 PS fixture 与指定 FFmpeg CLI；新增 GUI 卡顿用例随后定向 1/1 PASS，7.42秒 |
| ASan+UBSan 完整桌面 CTest | 9/9 PASS，156.13秒，433项QTest，零失败/跳过，LeakSanitizer 开启 |
| 生命周期 | 真实 SDL callback(dummy静音)→PCMA→WS、连接中取消、迟到201、重复停止、WS断开、握手超时、无效票据/设备、GUI卡顿不补发、窗口关闭等待、切平台精确清理 |
| Normal Qt→Go→实际C++ RTP | PASS，2.369秒，Qt发送101包，抽验60包的PT/SSRC/序号/160时间戳/精确静音payload；不是注入测试队列 |
| 同链路 Go race | PASS，3.169秒；Qt发送91包，9600样本持续到达，SIP BYE/200、owner结束、媒体404 |
| 实际数字测试音 | 私有Pulse源1000Hz，经真实SDL callback、FFmpeg、WS、C++ RTP后回解码；完整预声明窗口除首100ms外不挑片段；Normal RMS 0.349800、相关0.999987，ASan RMS 0.349615、相关0.999999 |
| 内部包搬移 | SHA校验后整体改到含空格路径；从 `/tmp` 启动实际 gb_monitor，XCB窗口、包内 Qt WebSockets/Core/FFmpeg maps、WM_DELETE 正常退出0；仅窗口/库/关闭，不冒充包内真实播放或麦克风验收 |

数字音频试验分别为 Normal 2.363秒、ASan 2.216秒；Qt采集/编码和 C++ sender 在 ASan 轮均插桩。外部 Qt/SDL/FFmpeg 系统或 SDK 库没有全量插桩，不能扩大第三方内存安全结论。C++ 服务的既有 makecontext/swapcontext warning 保留；所有轮次仅管理本轮 PID，没有影响系统 Pulse 或修改系统全局配置。

原始失败也保留：首轮桌面回归未传完整 PS fixture，且误用系统 FFmpeg，两个有限解码断言失败，解码器测试17项跳过，其他 PS 场景也未执行；改正执行环境后复验，不改断言。第一次私有 Pulse 缺独立 D-Bus，服务名冲突退出；另建独立 dbus-run-session 后成功。第一次搬移窗口探测在 headless 默认音频/标题查找组合下超时，未判 PASS；v2 使用 dummy 音频并按窗口所属 PID 查找，通过后正常关闭，不能据此唯一归因第一次失败。

## 自审与剩余边界

独立只读复审未发现可达 P0/P1，覆盖 SDL 关闭锁顺序、WS 同步重入、编码 buffer、固定平台身份与 POST/DELETE 寿命交接。新增状态仅为实际采集设备、编码器/帧/包、PCM队列及其锁、单调采集时钟、编码样本PTS、一次上传连接/定时器、在途HTTP及持有的talk_id；没有 started/closed/closing、额外线程、重试、数据库或 token 消费副本。

以下仍未完成，不宣称整个对讲或阶段六 GO：物理麦克风与相机扬声器、设备回传声音同时听音、Web采集与WSS发布、真实慢网络持续背压注入、物理采集设备拔出、第二机本轮采集，以及完整产品/公共发行验收。既有观看音频解码与本次上传分别有证据，但二者不能自动合成真实双向声学 PASS。

官方实现契约参考：[SDL capture/dequeue说明](https://wiki.libsdl.org/SDL2/SDL_DequeueAudio)、[Qt WebSocket API](https://doc.qt.io/qt-6/qwebsocket.html)。实际采用 callback 有界缓冲，未使用文档明确无界的 SDL 默认采集队列。
