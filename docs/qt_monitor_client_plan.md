# Qt 原生监控客户端实施计划

## 已确认范围

首发 Linux x86_64，后续适配 Windows；首版 H.264，H.265 后续。Qt Widgets 传统监控桌面布局遵循用户提供博客截图，Web 继续围绕 GB28181 监控主题；不重新制作通用直播后台。用户允许 FFmpeg/VLC 和现有协议/私有协议，由实测选择最简单方案。

当前采用 FFmpeg 解码、SDL2 PCM 输出、Qt Widgets 显示、现有 signaling API、RTSP/TCP。不维护第二个播放器，不为观看新增私有协议。平台负责 SIP 与共享 live，桌面客户端不再实现第二个 SIP 平台。

开发分支 `feature/qt-monitor-client`，独立工作区 `/home/gyl/example/media_server_qt`，开始基线 `ba803c2a8c0d2a82298dd9a9f68404cc0f57f320`。不改 main、其他 Agent、原型或第三方；每阶段真实测试后，与 DEV Claude Code 只读复审，解决阻断后中文提交并推送该开发分支，不自动合并。

## 七阶段

| 阶段 | 范围 | 必须满足的验证 |
| --- | --- | --- |
| 1 单路观看基础 | Qt 外壳、平台 API、设备树、GB 点播、普通源观看、H.264/AAC/PCMA/合法无音频、取消/超时、最小内部运行包 | 真实协议与双轨解码、数字音画标记/长时漂移、迟到回调隔离、正常及 sanitizer 测试、路径搬移；区分模拟 GB 和真实摄像机 |
| 2 多画面观看 | 1/4/9/16、选择/拖拽替换/关闭、声音、全屏/比例、偏好/抓图、主动 Catalog 入口 | 指定客户端硬件及明确媒体 profile，16 路同时真实实时解码；快速切换、取消、同源共享上游、单一路声音及资源回收 |
| 3 GB 控制 | DeviceInfo/DeviceStatus、PTZ、预置位 | SIP/XML、错误与超时；真实设备动作，模拟器不能冒充物理动作验收 |
| 4 录像检索回放 | RecordInfo、独立回放会话及控制 | 时间范围、无结果/超时、回放与实时所有权隔离、协议实际播放 |
| 5 下载及本地录制 | 设备下载、本地录制/抓取、进度/取消 | 可解码文件、磁盘/中断/取消、最终封装、资源和凭据清理 |
| 6 告警与音频业务 | 报警、广播、对讲 | SIP 事务、音频实际收发、冲突及取消、设备端验证 |
| 7 全局验收与交付 | 跨协议/多源、16 路、断网/替换/重启、sanitizer、运行包及第二机 | 可复核证据、依赖和许可边界、具体平台可搬运性；未执行项明确 BLOCKED/NOT_RUN |

阶段一加二是首个观看里程碑，不是完整平台完成。尚未定义“分发”业务保持禁用，不造空接口。Qt 退出只关闭自己观看资源，不停止后台服务或其他观众的 GB 上游。

## 实现与验收原则

- 复用现有 source generation、一次性授权和媒体输出，不恢复 reader/history/GOP replay 或反向 viewer 控制。
- 每次播放独立票据；认领成功永久消费，不回滚。GB 媒体就绪信息从既有对账派生，不新增后台状态、持久字段或另一个 registry。
- QObject context 隔离请求代际；FFmpeg 网络/解码在工作线程，QWidget/SDL 在 GUI。停止中断旧线程并保活到退出，不在 GUI 等待网络。
- 各实际媒体轨道分别有进展超时；真实坏包/协议错误明确结束，不无限重试。只有首开业务阶段可以有总期限的事实就绪检查。
- 有界视频/PCM/网络缓冲，偏好状态和必要媒体时钟可保存，不加冗余 started/closed/通用生命周期状态。
- 不把 HTTP 成功、收到字节、RTP 计数、PCM 增长等同于画面/音频完整验收。屏幕采集与数字声音回环另列，不能泛称物理声学测试。
- 16 路结果绑定 CPU、解码方式、分辨率/帧率/GOP/码率与音轨，不以 16 个黑框或 16 次签票代替。
- 内部实验依赖和正式发行依赖分开。正常服务器 SDK 与系统安装保持不动，独立 decoder SDK/包保留配置、来源、许可及 SHA。
- 每阶段保留 RED/GREEN 和原始证据；清理只针对本轮已核对身份的进程/可再生中间产物，不清理他人资产或历史原始数据。

## 当前检查点（2026-10-15）

阶段一完成开发与定向验收：已验证真实 GB 模拟设备的冷点播、H.264/PCMA、AAC/无音频、请求/解码取消和新旧回调隔离；GB 冷首开旧 RED 保留，新增派生 `live.media_ready` 后不重签票即可正常播放。最终 shared SDK 正常和 ASan+UBSan 桌面 CTest 均4/4通过（39.01/39.14秒），Go 全量 test/race/vet 通过；后续修改须重新验证，不能继承本次结论。

音画同步采用独立 X11+私有 Pulse 数字回环；最终 shared SDK 代码的 AAC 11 个标记（含首个）偏差 −20.500～+0.500ms，PCMA 10 个标记偏差 −19.521～−9.521ms。10 分钟 AAC 的稳态漂移另有证据，不替代真实设备扬声器，也不抹去旧起播偏差。反复关闭/重开音频设备的切换停顿已用同会话 Pause/Clear 复用修复，16 秒严格切换验证通过；停止仍关闭设备（阶段一行为，阶段二已改为归还窗口设备借用）。真实 ASan AAC/PCMA/冷GB播放已验证，内部包搬移/依赖及包内实际 GUI 播放、声音开关、正常窗口关闭通过，GUI持续观察10.846秒。Claude Code 第10/11轮复审提出的阶段一门槛已兑现；详情见 [阶段一验证](qt_monitor_client_stage1.md)。内部包不等于公共分发许可已闭合。真实第二机、16 路和后六阶段不继承历史 PASS。

原始私有证据目录：DEV `/var/tmp/media-server-qt-stage1-bFmYuf/`；GB `/tmp/qt-gb-plan-WDjxT3VH/`；Claude 会话记录 `/tmp/qt-client-plan-claude-JuZHjy/`。原始日志可能含授权，不提交 Git，也不公开复制。

阶段二完成：1/4/9/16布局、选择/拖放替换/停止、唯一音频设备借用、全屏/比例/抓图/偏好及主动Catalog。i7-13700KF上的同源16路独立RTSP/CPU解码，H.264 Main1080p25约4Mbps+AAC，30秒共同窗口每路749帧、双轨持续进展；不是16不同源或正式容量基线。实际双音源50次切声、GB四格共享唯一INVITE/停三留一、viewer退出不停止上游通过。最终桌面normal及ASan+UBSan各4/4 CTest（各61个QTest、无失败/跳过），Go全量test/race/vet通过；内部包整体搬移、包内实际GUI播放/声音/正常退出通过。详情及原始失败边界见 [阶段二验证](qt_monitor_client_stage2.md)。

阶段二证据：DEV `/var/tmp/media-server-qt-stage2-main-7Do5ny4N/`、`/var/tmp/media-server-qt-stage2-GwavyowG/`、`/var/tmp/media-server-qt-stage2-audio50-VlVrz8/`。当前观看里程碑不等同完整GB平台GO。

阶段三的设备查询子集已实现：DeviceInfo/DeviceStatus 使用短寿命 SN 关联独立 SIP/XML 响应，Qt 基本页显式查询、展示设备自报结果，选择通道仍查父设备；不新增持久状态、轮询或媒体控制。最终 normal 和 ASan+UBSan 桌面各 4/4 CTest、77 项 QTest，无失败/跳过；Go 全量 test/race/vet 通过，保留首轮既有 SQLITE_BUSY 失败及原基线复现。实际 Qt→Go HTTP→UDP 受控 peer 查询与界面显示通过，不等于物理摄像机验收。详见 [阶段三查询子集](qt_monitor_client_stage3_queries.md)。

阶段三证据：DEV `/var/tmp/media-server-qt-stage3-main-Y4xy1xyZ/`、`/var/tmp/media-server-qt-stage3-query-RjkDSe9c/`、`/var/tmp/media-server-qt-stage3-window-tests-I1Pb7bPn/`。PTZ/预置位仍未实现，真实可动云台、预置位及录像设备条件待确认；不能把这个子集标记为整个阶段三完成。
