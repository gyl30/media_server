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
