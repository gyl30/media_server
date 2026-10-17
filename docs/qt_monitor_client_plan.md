# Qt 原生监控客户端实施计划

## 已确认范围

首发 Linux x86_64，后续适配 Windows；首版 H.264，H.265 后续。Qt Widgets 传统监控桌面布局遵循用户提供博客截图，Web 继续围绕 GB28181 监控主题；不重新制作通用直播后台。用户允许 FFmpeg/VLC 和现有协议/私有协议，由实测选择最简单方案。

当前采用 FFmpeg 解码、SDL2 PCM 输出、Qt Widgets 显示、现有 signaling API、RTSP/TCP。不维护第二个播放器，不为观看新增私有协议。平台负责 SIP 与共享 live，桌面客户端不再实现第二个 SIP 平台。

2026-10-16 用户确认复用既有大华 `172.20.44.240` 做 GB28181 实机验证，并尽可能支持标准定义的功能。按[能力补齐与实机矩阵](gb28181_device_capabilities.md)区分现实现、2016/2022 标准、设备能力与真实验收；先只读查询，再分批补齐回放控制、订阅及音频链路，不把可选能力缺失或测试凭据阻塞当作设备功能失败。

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

## 当前检查点（2026-10-16）

阶段一完成开发与定向验收：已验证真实 GB 模拟设备的冷点播、H.264/PCMA、AAC/无音频、请求/解码取消和新旧回调隔离；GB 冷首开旧 RED 保留，新增派生 `live.media_ready` 后不重签票即可正常播放。最终 shared SDK 正常和 ASan+UBSan 桌面 CTest 均4/4通过（39.01/39.14秒），Go 全量 test/race/vet 通过；后续修改须重新验证，不能继承本次结论。

音画同步采用独立 X11+私有 Pulse 数字回环；最终 shared SDK 代码的 AAC 11 个标记（含首个）偏差 −20.500～+0.500ms，PCMA 10 个标记偏差 −19.521～−9.521ms。10 分钟 AAC 的稳态漂移另有证据，不替代真实设备扬声器，也不抹去旧起播偏差。反复关闭/重开音频设备的切换停顿已用同会话 Pause/Clear 复用修复，16 秒严格切换验证通过；停止仍关闭设备（阶段一行为，阶段二已改为归还窗口设备借用）。真实 ASan AAC/PCMA/冷GB播放已验证，内部包搬移/依赖及包内实际 GUI 播放、声音开关、正常窗口关闭通过，GUI持续观察10.846秒。Claude Code 第10/11轮复审提出的阶段一门槛已兑现；详情见 [阶段一验证](qt_monitor_client_stage1.md)。内部包不等于公共分发许可已闭合。真实第二机、16 路和后六阶段不继承历史 PASS。

原始私有证据目录：DEV `/var/tmp/media-server-qt-stage1-bFmYuf/`；GB `/tmp/qt-gb-plan-WDjxT3VH/`；Claude 会话记录 `/tmp/qt-client-plan-claude-JuZHjy/`。原始日志可能含授权，不提交 Git，也不公开复制。

阶段二完成：1/4/9/16布局、选择/拖放替换/停止、唯一音频设备借用、全屏/比例/抓图/偏好及主动Catalog。i7-13700KF上的同源16路独立RTSP/CPU解码，H.264 Main1080p25约4Mbps+AAC，30秒共同窗口每路749帧、双轨持续进展；不是16不同源或正式容量基线。实际双音源50次切声、GB四格共享唯一INVITE/停三留一、viewer退出不停止上游通过。最终桌面normal及ASan+UBSan各4/4 CTest（各61个QTest、无失败/跳过），Go全量test/race/vet通过；内部包整体搬移、包内实际GUI播放/声音/正常退出通过。详情及原始失败边界见 [阶段二验证](qt_monitor_client_stage2.md)。

阶段二证据：DEV `/var/tmp/media-server-qt-stage2-main-7Do5ny4N/`、`/var/tmp/media-server-qt-stage2-GwavyowG/`、`/var/tmp/media-server-qt-stage2-audio50-VlVrz8/`。当前观看里程碑不等同完整GB平台GO。

阶段三的设备查询子集已实现：DeviceInfo/DeviceStatus 使用短寿命 SN 关联独立 SIP/XML 响应，Qt 基本页显式查询、展示设备自报结果，选择通道仍查父设备；不新增持久状态、轮询或媒体控制。最终 normal 和 ASan+UBSan 桌面各 4/4 CTest、77 项 QTest，无失败/跳过；Go 全量 test/race/vet 通过，保留首轮既有 SQLITE_BUSY 失败及原基线复现。实际 Qt→Go HTTP→UDP 受控 peer 查询与界面显示通过，不等于物理摄像机验收。详见 [阶段三查询子集](qt_monitor_client_stage3_queries.md)。

阶段三设备查询证据：DEV `/var/tmp/media-server-qt-stage3-main-Y4xy1xyZ/`、`/var/tmp/media-server-qt-stage3-query-RjkDSe9c/`、`/var/tmp/media-server-qt-stage3-window-tests-I1Pb7bPn/`。预置位只读查询单独增量开发，见 [预置位查询验证](qt_monitor_client_stage3_presets.md)。后续设置、调用、删除见下方独立子集记录；真实可动云台、预置位及录像设备条件待确认，不能把协议子集标记为整个阶段三完成。

预置位只读查询已通过 normal 与 ASan+UBSan 各 4/4 桌面 CTest（各106项 QTest，无失败/跳过，63.53/63.30秒）、Go 全量 test/race/vet 和实际 Qt→HTTP→UDP 组合；保留首轮既有 SQLite BUSY 与私有 OCR 工具失败。当前只支持父注册设备 From、单个完整 PresetList，编号按协议字符串保留；不套控制指令数值范围，不新增 Catalog 聚合。新增状态仅为在途查询的 targetID，Qt复用原查询context。证据：DEV `/var/tmp/media-server-qt-stage3-preset-92XuAF4R/`、`/var/tmp/media-server-qt-stage3-presets-main-UakD2P0j/`、`/var/tmp/media-server-qt-stage3-preset-tests-w8PH80ui/`。

有界云台点动与STOP协议子集已实现：300毫秒START/等待后，独立1.5秒STOP尝试；重复STOP、注销及HTTP取消不取消STOP，结果只确认SIP受理。唯一新增运行事实是通道owner CancelFunc map，不新增DB状态或关闭标志。修复并真实RED/GREEN验证HTTP排空期间SIP过早关闭；最终Go全量test/race/vet通过，normal与ASan+UBSan各4/4桌面CTest、150项QTest（73.32/75.39秒）无失败/跳过。实际Qt→HTTP→UDP点动/STOP、界面结果及退出通过；物理云台/变倍/停止仍BLOCKED，不能以受控peer代替。详见[点动与停止验证](qt_monitor_client_stage3_ptz.md)。证据：DEV `/var/tmp/media-server-qt-stage3-ptz-TcYTJA2n/`、`/var/tmp/media-server-qt-stage3-ptz-main-8CZY6OxW/`、`/var/tmp/media-server-qt-stage3-ptz-tests-SERSNLBH/`。

预置位设置/调用/删除子集已实现，复用原PTZ入口与通道owner，不新增DB或移动状态；SIP受理不等于到位。设置/删除需默认No非模态确认，调用/删除只接受查询中原样可编码的1–255编号，STOP保持独立。Go全量test/race/vet及最终normal与ASan+UBSan各4/4桌面CTest（各191项QTest、81.99/82.82秒）通过；实际Qt→HTTP→UDP组合run04通过，取消设置零报文、三条控制顺序及编码、owner/pending归零、正常退出均验证。保留三轮私有GUI操作脚本失败，Claude第24/25轮设计/代码复审无阻断。物理保存/删除/到位及STOP中断goto仍BLOCKED。详见[预置位控制验证](qt_monitor_client_stage3_preset_control.md)。证据：DEV `/var/tmp/media-server-qt-preset-control-sIcW6NEo/`、`/var/tmp/media-server-qt-stage3-preset-control-main-3OjFqtkm/`、`/var/tmp/media-server-qt-stage3-preset-control-tests-PQLhWkD7/`。

阶段四录像目录查询子集已实现：新RecordInfo入口复用SN/pending与锁，累计完整分批后返回，标准零条及无Result支持，保留合法重复及接收顺序。应用上限1000条/平台15秒/Qt20秒，不新增DB或后台轮询；独立非模态窗口取消先断回调再abort，不干扰实时或PTZ。Go全量test/race/vet通过，normal与ASan+UBSan各5/5桌面CTest（各311项QTest，109.01/111.14秒）通过；实际Qt→HTTP→UDP组合run02显示两批目录、期末pending/live/owner/INVITE均0、正常退出。Claude第26–29轮设计及生产复审无阻断，原始工具/fixture失败保留。真实录像目录BLOCKED，独立历史回放/下载NOT_RUN，整个阶段四及平台仍未GO。详见[录像目录查询验证](qt_monitor_client_stage4_record_query.md)。证据：DEV `/var/tmp/media-server-qt-record-query-zk3R88Gs/`、`/var/tmp/media-server-qt-record-ui-tests-WtTP1nRN/`、`/var/tmp/media-server-qt-stage4-record-main-6S4nzeFB/`。

录像回放前置审查发现既有实时 INVITE 取消期间的成功响应未被 ACK/BYE、以及重传 ACK 的 CSeq/Route 错误，先独立修复，不复制缺陷到回放。平台改用公开事务单 owner 等待、Init 前安装 ACK 重传回调和同一窄对话请求构造器；不新增对话 registry、后台 goroutine、持久状态或关闭标志。详见[邀请取消与 ACK 生命周期](qt_monitor_client_sip_lifecycle.md)。该修复不代表历史回放已实现；录像首尾、独立会话所有权及实际控制仍须单独设计和验证。

阶段四继续采用独立有限传输，不把历史录像接入实时 `media_stream`：TCP/RTP 原始 PS 通过一次授权 HTTP chunked 通道交给 FFmpeg。传输正常结束与录像内容完整严格区分；私有真实媒体组合已验证逐帧及 PCM 参考一致和异常结束，自然注销及 sanitizer 结果单列。详见[有限录像传输子集](qt_monitor_client_stage4_finite_transport.md)。signaling 历史 owner/票据/INVITE/121/INFO、Qt 有限背压及显示和音频尾部排空仍须后续实现，不把这个组件标记为完整历史回放。

接续增量已实现独立历史信令 owner、GET 一次认领后 INVITE、Playback SDP、121/远端 BYE 的有界收尾与失败补偿。详见[历史回放信令 owner 增量](qt_monitor_client_stage4_playback_owner.md)。当前 Go 全量 test/race/vet 通过；同一源码实际实时四格 193 项断言通过，独立历史 normal/ASan × AAC/PCMA 四组逐帧/PCM 精确一致、资源归零。解码组件不是 Qt 有限呈现验收，INFO、暂停/恢复/seek、下载、物理 NVR 与后续阶段仍未完成。回归另外稳定复现并最小修正原推流 SQLite 错误返回身份的合同缺陷，保留原失败并按独立逻辑审查提交，不改变原子消费 SQL 或忙锁策略。原 Claude 第 51/52 轮因额度限制退出，未当作批准；提交前由本机 Codex 独立只读复审两轮（含认领/DELETE、ACK 后结束、对账 Stopping、SSRC 释放与 201 写出时序的逐条追问），SQL 修复与历史 owner 均无 P0/P1。

后续 Qt 有限呈现增量已经接上目录选项、固定 UTC 偏移、独立历史创建与 DELETE、有限视频/PCM 背压及尾部排空；未增设第二套播放器。Qt 6.2.4 对缺少 HTTP chunked 终止块的完成判断存在独立 RED，因此有限输入用既有 Boost.Beast 头文件解析真实分帧，正常 TLS 校验不变。主窗口排队回放与零字节响应断言两个提交前修复均有真实 RED；最终 normal 与 ASan+UBSan 桌面各 6/6 CTest、各 367 项 QTest，无失败/跳过，134.94/137.37 秒；真实 Qt→Go→SIP→C++→有限呈现 normal/ASan × AAC/PCMA 四组完成 50 帧全序列匹配与资源回收。保留目录清空选择、AAC padding 参考及原夹具失败，不以静音或 SDL dummy 冒充数字/物理音频验收。详见[Qt 有限录像呈现增量](qt_monitor_client_stage4_finite_view.md)。本机 Codex 三轮只读复审后无 P0/P1。INFO 控制、真实录像设备和阶段五至七未完成，不能宣称整阶段四或平台 GO。

阶段四同会话暂停/恢复已实现：Go 在原 dialog 内发送 GB/T 28181—2016 附录 B 的 MANSRTSP INFO，INFO/BYE 同门串行并递增 CSeq，忙即拒绝、结果未知不自动补发相反指令，暂停仍消耗原绝对预算；C++ 有限通道删除正文写期限以免合规设备的长暂停被 20 秒断开，停读消费者改由 owner 期限回收；Qt 平台受理后冻结时钟与 SDL，解码进展期限不计暂停区间。C++ normal/ASan 各 69/69，Go 全量 test/race/vet，桌面 normal/ASan 各 6/6、372 项 QTest；真实 Qt→Go→SIP→C++ normal/ASan × 静音/SDL dummy 四组暂停 22 秒后 200/200 帧逐帧一致。保留 UBSan 析构回调真实失败、私有夹具失败与既有预置位测试不稳定。seek、倍速、物理 NVR 与阶段五至七仍未完成。详见[有限传输暂停背压](qt_monitor_client_stage4_finite_transport.md)、[历史 owner 的 INFO](qt_monitor_client_stage4_playback_owner.md)与[Qt 暂停与继续](qt_monitor_client_stage4_finite_view.md)。

阶段五本地录制与设备下载子集已实现：新流复制录像组件把实时 RTSP 或有限 PS 复制为 Matroska，`.part` 收尾成功才改名；桌面 SDK 只追加 Matroska muxer 与 extract_extradata。Go 新增 `s=Download`/`a=downloadspeed` 下载入口，复用历史 owner；录像窗口下载、实时画面本地录制、退出等待录制线程。桌面 normal/ASan 各 7/7、393 项 QTest，Go 全量 test/race/vet；真实下载六组与本地录制八组（normal/ASan）通过。Codex 发现的映射越界/静默丢音频、收尾取消、损坏包与退出线程风险均已修正并保留 RED。物理 NVR 下载、设备倍速兼容、磁盘满实测未做。详见[阶段五录制与下载](qt_monitor_client_stage5_record_download.md)。

阶段六报警子集由 Codex 接手 Claude 未提交实现后完成：已注册设备/所属通道通知、独立 Response MESSAGE、有界近期缓存及 Qt 增量列表；修正重启序号追平旧游标时漏报和切平台旧请求未取消的真实边界。桌面 normal/ASan+UBSan 各 7/7、396 项 QTest，Go 全量 test/race/vet 通过；实际 Qt→生产 HTTP→UDP SIP normal/ASan 各一组通过，不代表物理摄像机。新增运行实例身份仅服务单一游标，不增加持久化报警或关闭状态。用户已要求停止 Claude 及其自动续接，本子集由 Codex 实施与独立只读复审。详见[阶段六报警](qt_monitor_client_stage6_alarms.md)。

阶段七部分复验已执行：最新桌面源码同源16路1080p25 H.264/AAC独立解码，共同30秒每路750帧、音视频逐秒进展、关闭与线程回收通过。clean源码 `a2da6dd` 内部包在DEV完成含空格路径搬移、包内Qt/XCB加载与正常关闭；第二机完成SHA、依赖、offscreen加载，并通过独立认证SSH/Xvfb验证TEST实际XCB窗口及正常关闭。第二机实际播放/声音未验收。完整数据、工具失败、SHA与边界见[Codex接手与阶段七部分验收](qt_monitor_client_takeover_verification.md)，不是正式容量或物理设备结论。

后续真实缺口：同会话 seek/回放倍速、广播/对讲仍未实现，整个阶段六未完成。seek 需要已验证的媒体边界（例如有效 RTP-Info）并明确传给 Qt 刷新解复用/解码/PCM，不能靠猜 PTS 或重开会话替代；广播/对讲需要采集、纯音频媒体方向及真实设备端验收，现有视频 PS sender 不等同音频链路。阶段七第二机本地桌面/播放/声音、物理设备与公共分发许可边界尚未闭合，不能据观看回归与跨机窗口验证宣布整个平台 GO。

大华广播入口已完成私有取证：主设备目标返回独立 Broadcast Result=OK 并反向 INVITE，提供 recvonly PCMA/8000 PT8。修正私有 UAS handler 提前返回、并补全原事务 ACK 路由后（本轮两条真实 ACK 都走顶层路由），真实 200/ACK、500 包/10秒近零静音 UDP 写出、正常 BYE 和端口释放通过；原失败保留。未收到相机 RTCP，不等于设备收包、解码或听音通过；Qt采集、跨主机直送与正式广播/对讲仍未实现。首发候选方案为 Qt 作为真实输入、Go SIP控制的直连LAN直送，以跨主机设备实际收音为采纳门槛，不提前排除必要的中继。详见[大华广播入口取证](qt_monitor_client_dahua_broadcast.md)。

广播 SDK 前置能力已独立验证：FFmpeg 9.0.1 新前缀只追加 PCMA encoder/RTP mux，编码、分包、回解码、compound RTCP 及模拟 EIO 两模式通过；旧桌面 normal/ASan+UBSan 各8/8、422项QTest零失败/跳过。首轮缺素材49项跳过的记录保留为INCOMPLETE，不覆盖原SDK；仅清理本轮可再生FFmpeg中间对象。此结果不代表已实现Qt广播，也不是UDP/麦克风/相机声音验收。配置、SHA、边界见上述取证文档第9节。

跨IP补充取证已完成一次：SIP在TEST、PCMA/RTP从DEV发送400包，真实ACK/BYE及资源回收通过；同时采集的实时PS无观测RTP序号缺口，H.264解码577帧、PCMA解码23.04秒。音频PTS不均匀且五段测试音未全部达到预设关联条件，结果INCONCLUSIVE；保留重复DTS警告及非验收诊断，不把本地发送或部分频带增强写成相机声音通过。相机配置前后查询一致，Qt/Go正式广播仍未实现，详见取证文档第10节。

后续音量对照仅临时将AudioOutputVolume[0]从97改0，异常后已恢复97并GET核实，这是配置恢复而非第三轮媒体复验。第一轮原音量媒体采集/解码完成但严格分析仍INCONCLUSIVE；静音轮重启私有SIP平台后120秒内注册/目录未就绪，未授权或发送音频，第三轮停止未做。完整对照INCOMPLETE、离线对照INCONCLUSIVE，不能用0包充当静音负对照。所有失败和配置读写保留；原最终Claude复审额度失败，不算批准，接手后已独立只读核对并补齐证据校验。下一步应采用稳定单一控制会话，不更改生产注册规则或旧音频门槛。详见取证文档第11节。

真实大华接入后新增呈现缺陷复验：原四路一画面解码增长但呈现停滞，诊断16路亦复现墙钟落后于不断淘汰的实时视频队列。局部淘汰标记配合既有墙钟重锚修复，不新增成员或更改音频主时钟/有限回放；公共play→真实RTSP/H.264的静音及选中无音轨RED/GREEN已建立。真实相机同源四路和16路分别完成30秒共同观察、逐路音视频/呈现增长及停其余留一路，ASan四路带SDL dummy输出亦通过；不是16不同源、物理屏幕/声音或完整GB平台验收。注册、目录、心跳、信息/状态/预置位查询已实测，录像查询指定窗口为空，不代表录像能力失败。详见[大华实机与呈现饥饿修复](qt_monitor_client_dahua_rendering.md)，保留原始失败及测试环境边界。

大华云台实机补充：HTTP→真实SIP→设备独立状态证明左右点动后停止，临时预置位3保存、调用归位与删除均有效，最终三坐标残差0且原预置位列表恢复；这些实机证据更新前述相应BLOCKED状态。已抓包的右点动和两次变倍START的200迟于300毫秒，现有接口按安全合同返回502；左点动同样返回502，但不在抓包覆盖内。未延长动作或保留STOP后的START重传。变倍未观察到实际坐标变化，STOP中断goto及Qt按钮人工实机验收未做，不能写为完整PTZ PASS。详见[实机云台与预置位](qt_monitor_client_dahua_controls.md)。证据DEV `/var/tmp/qt-dahua-controls-md9w0l/`、TEST `/var/tmp/qt-dahua-a2fcc80-V2mC1Ksf/run-06-controls/`；本子集不修改生产实现。

大华库存录像实测补充：RecordInfo 找到既有录像，真实 TCP 末媒体后 19.878584 秒才 FIN；旧 Qt 回放只完成246/247帧、下载误报10秒无进展。现复用平台已有绝对 `deadline`，只限制未完成的有限HTTP；完整后允许本地解码/暂停排空，实时双轨10秒规则不变，删去旧暂停进展时基。Normal及ASan+UBSan桌面各8/8、各417项QTest通过；同一设备范围两模式回放均247帧逐帧匹配、PCM计数完整，两模式下载重解码帧与PCM hash一致并完成DELETE。更新的是该设备与该库存范围的真实回放/下载边界，不回写历史受控peer为物理验收；seek、倍速、广播/对讲、物理声画及完整平台仍未GO。详见[有限录像期限修正](qt_monitor_client_history_deadline.md)。

PTZ 错误身份增量：START 超时/取消且独立 STOP 成功现返回 `ptz_start_unconfirmed`，Qt 显示动作受理未确认与 STOP 协议受理，其他事务错误、STOP 优先级及预置位不变。已建立真实 UDP 迟到响应屏障和 Qt 文案 RED/GREEN，无新生产状态、helper、timer 或迟到响应 registry；没有延长300ms或改变STOP顺序。本项仅解决操作员误判，不消除真实相机迟到200的成功窗口限制。完整验证和边界见[实机云台文档的分类增量](qt_monitor_client_dahua_controls.md)。

同会话回放控制取证：生产 SIP/dialog 加私有 TCP collector 实测向前/向后定位及 2×/1×四次 INFO，真实 PTS 跳变、SPS/PPS/IDR 和发送节奏变化已有证据，RTP 序号保持连续，但设备未返回 RTP-Info；新位置媒体的应用层记录还早于控制完成。跨定位原始 PS 直接拼接存在解码诊断，不能把 INFO 200 或工具退出0当作 Qt seek/倍速通过。本项未修改生产代码，相机配置未改，owner/SSRC/端口清理通过；客户端同会话 seek/倍速仍未实现，详见[大华回放控制取证](qt_monitor_client_dahua_playback_controls.md)。

2026-10-17接续完成广播内部SIP控制owner：SQLite高水位SourceID、Notify/独立Response/反向INVITE与双ACK路由、单设备占用、会话结束通知及统一清理。删除HTTP取消提前撤设备fence有真实RED/GREEN。最终Go全量test/race/vet通过；修正后真实大华在单一SIP进程连续完成尾号4/5两次控制并归零，沿用原DB，不发送音频或SET配置。DEV Claude最终只读复审无P0/P1。此增量尚无公开HTTP、租约、Qt采集或广播UI，未采纳直送/中继路线，也不改变旧广播INCOMPLETE/INCONCLUSIVE和平台未GO结论。边界、SHA及证据见[广播内部控制owner](qt_monitor_client_stage6_broadcast_control.md)。

第二机实际播放补验：clean基线347decb的真实Qt窗口探针在TEST通过大华单路12秒及同源四路30秒，75/574项断言零失败；四路停三留一继续呈现，全部进程正常回收。私有Pulse记录到归属于Qt PID的非零PCM，认证SSH/Xvfb截图确认实际相机画面；它们是数字音频和虚拟屏幕，非物理听音/显示或安装版程序外部播放操作。原scp遇UNIX socket的外层INCOMPLETE记录保留，另行补收普通文件与SHA。未改生产实现或重跑全套回归，公开分发和完整平台仍未GO，详见[第二机播放补验](qt_monitor_client_stage7_second_play.md)。

内部包追溯增量：修正CMake编译器路径为STRING时版本漏记，完整打包CLI真实RED/GREEN通过，准确记录Ubuntu GCC16.0.1 experimental。三份实际GCC运行库另与本地16.2.0构建产物逐字节匹配，签名源码归档及有限源码核对保留；不把编译器与运行库版本混同，不据此关闭项目许可证、源码交付或公开分发门槛。详见[运行包来源补证](qt_monitor_client_package_provenance.md)。

稳定单一SIP平台下的音量对照已接续：首轮监督工具完成标记竞态保留INCOMPLETE，实际AST回归RED/GREEN修正后另建独立实验，三轮协议/采集/配置及97恢复完整，原比较器音量来源验证true。原严格PTS前提和完整五频/10dB条件仍未满足，比较仍INCONCLUSIVE；不能据部分频带变化采纳直送路线或宣布广播声音PASS。旧失败、源码、ELF、媒体与高水位连续性证据均保留，详见[单一平台音量对照](qt_monitor_client_stage6_stable_volume.md)。

录制实际ENOSPC补验完成：独立namespace的1MiB tmpfs、余32KiB下，Normal和ASan+UBSan均在已处理音视频包后写盘失败、不保存且删除本次.part，线程/挂载回收；Normal有内核ENOSPC轨迹，ASan以独立文件系统对照确认并保留LSan。首次私有文案断言错误保持INCOMPLETE，修正后不改生产EIO合同；12:22恢复只补未执行的ASan，不重跑已通过Normal。未扩大为真实设备下载或全量回归，见[实际磁盘满补验](qt_monitor_client_record_enospc.md)。
