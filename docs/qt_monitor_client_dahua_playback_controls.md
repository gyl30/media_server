# 大华同会话回放控制取证

## 1. 目的与范围

2026-10-16，在 `d3d364973ecefc59bfe2373a0a4a7e239da8d1b2` 上确认真实大华回放的向前/向后定位及 Scale 行为。此次只有生产 SIP/dialog 与私有 TCP/RTP collector，没有经过生产 PS→HTTP 接收器或 Qt 播放器，**不是客户端 seek/倍速验收**，也没有修改生产代码。

相机为 `172.20.44.240`、设备 `34020000001320000089`、通道 `34020000001310000089`。复用库存录像 2026-10-16 07:11:38–07:12:24（UTC+8），SDP `t=1792105898 1792105944`，不新建或删除录像。视频 H.264 Main 1920×1080，音频 PCMA 8000 Hz 单声道。本次没有调整相机配置。

## 2. 方法与隔离

- DEV 使用 Go 1.27.2 编译私有 Go test，`go -overlay` 将工作区外的 `_test.go` 加入当前 signaling 包；仓库中不增加测试入口。TEST 只运行二进制，不编译源码。
- 真实 REGISTER/Catalog 后，复用 `newSIPServer()`、`newLiveService()`、`buildGBSDP()`、`live.invite()`、`session.nextDialogRequest()` 与生产停止流程。全部 INFO/BYE 由同一 `session.dialog` 串行分配 CSeq；`established` 恰好关闭一次。
- 私有媒体控制 adapter 只应答 SSRC update/delete，关闭自己的 collector；它不是实际 media_server。接收真实 RFC4571 RTP，保留原始包，并仅将合法、非空 PT96 payload 写入 PS，不以序号跳变截断取证。
- SIP 使用相机现有平台 `172.20.63.32:15060`，测试前确认空闲；collector 使用自有 `38440/TCP`。凭据只从仓库外 0600 文件进入内存，不进入 argv、文档或提交。
- 私有 session 预算为 70 秒，不代表生产历史授权期限。测试从启动到退出 48.60 秒；UTC 10:17:20–10:18:09。起播后依次等待 5 秒再发送各控制，最后再观察 6 秒并主动停止，没有重开 INVITE。

请求参考 [GB/T 28181—2016 附录 B](https://ossrs.net/lts/zh-cn/assets/files/GBT28181-2016-3f992e659a46001a1c89c05db539a5d3.pdf)：`PLAY RTSP/1.0`、内层 CSeq 与外层相同，单次只携带 Range 或 Scale。没有同时携带两者，应用层没有重发控制；SIP UDP 事务仍可能按协议重传同一请求，详见下述限制。

## 3. 实际响应与媒体变化

四次响应均为 SIP 200，内层均为 `RTSP/1.0 200 OK` 且 CSeq 对应；正文只有状态与 CSeq，外层也没有 Range、Scale 或 RTP-Info。**没有收到标准所描述的 RTP-Info 边界。**

| 控制 | `client.Do()` 完成耗时 | 实际媒体观察 |
| --- | ---: | --- |
| `Range: npt=20-` | 771.599 ms | 视频 PTS 从相对 5.270 秒跳至 20.270 秒，跳变 +15 秒；首帧为 keyframe |
| `Range: npt=5-` | 590.600 ms | 视频 PTS 从相对 25.839 秒跳至 6.839 秒，跳变 −19 秒；首帧为 keyframe；不能将 6.839 秒宣称为精确落到 5 秒 |
| `Scale: 2.0` | 400.502 ms | 短稳态窗内视频 PTS/采集单调时间 OLS 斜率约 1.897，明显快于正常发送；不是精确倍速误差或 Qt 呈现验收 |
| `Scale: 1.0` | 329.895 ms | 同口径斜率约 0.997，恢复接近正常发送节奏 |

相对 PTS 以首个可映射位置的视频包 PTS 为零点，不作为摄像机录像绝对时刻。控制后速率估计使用响应后 1 秒至下次响应前 0.2 秒的短窗；初始窗口从首个 RTP 后 1 秒开始，最后的 1×窗口截止于最后一个 RTP 前 0.2 秒，而非不存在的下一次响应。保护窗口只用于计算速率，**不作为业务切换边界**。没有重复试验或置信区间，不能扩大为所有录像/所有倍率结论。

共采集 14,552 个有效 PS-RTP 包、17,137,727 字节 PS。SSRC 恒为 `1200000001`，观测序号跳变为 0；同一个已接受 TCP 连接持续交付四次控制前后的媒体。collector 本身只接收一个连接，本次没有出现首连接结束后重连；不据此声称已覆盖重连设备。

ffprobe 映射到 808 个视频包、538 个音频包；PS 两处跳变后的 PES offset 分别为 2,947,085 和 5,964,945。在两处都逐字节检查到 PES 前的 pack/system/PSM 起始码，以及第一段视频 payload 内按序出现的 SPS、PPS、IDR 起始头；不是完整解析校验所有 PS 结构。不能仅从 keyframe 标志推断这些头部存在。

## 4. 为什么不能在 INFO 200 后简单刷新播放器

向前定位的新 PTS 在 collector 中记录于本次启动后 25.534481 秒，INFO 完成记录为 25.726702 秒，早约 192.221 ms；向后定位分别为 31.307032 与 31.318056 秒，早约 11.024 ms。

这是同一进程单调时间的**应用层记录顺序**，不是内核抓包时间；不能据此精确推断 UDP 响应到网卡与 TCP 新数据到网卡的先后。但它已经说明：调用方收到 INFO 完成时，接收/排队路径可能已经处理新位置媒体。仅在 HTTP 控制成功后清空 Qt 队列不能标识这条边界，更不能把成功响应后的首包无条件当成新位置首包。

本次取证时 `signaling/gb_playback.go::controlPlayback()` 只支持 pause/resume，不传 RTP-Info；`media/gb28181/gb28181_playback_session.cpp` 的接收循环校验连续 RTP 并只向 HTTP 输出 PS payload；`desktop/finite_http_input.cpp` 不接收 RTP 元信息。本次设备序号连续不代表其他设备相同，更不构成删除连续性校验的理由。后续 Range/Scale 信令增量见[历史 owner 文档](qt_monitor_client_stage4_playback_owner.md)，不回写为本次已测功能。

## 5. 解码结果与明确失败边界

直接将跨 seek 的 PS 拼成一个文件，ffprobe 与 FFmpeg 都退出 0，但原始 stderr 存在 DTS 倒退、timestamp discontinuity、packet corrupt，FFmpeg 还报告 H.264 宏块解码错误。测试尾部由本地主动停止，不是自然完整录像 EOF。

这些日志全部保留；没有把非零诊断吞掉，也没有将退出 0 写成无错误双轨解码 PASS。此次未将各诊断精确归因到 seek 边界还是主动截断尾部，不能宣称全部只是尾部警告。采集的音频包证明音轨存在，不证明跨定位后的 PCM 连续性、正确刷新或实际声音输出。

## 6. 清理与审查

使用生产 `stopPlaybackID()`，媒体替身收到 delete 后关闭自有 listener/socket；随后 collector 排空，再停止 SIP。最终 session 数 0、SSRC allocation 数 0，测试退出 0，原 PID 消失，15060/38440 不再监听。SIP 事务层记录了未匹配的 200，并提示可能为重传；没有逐条关联其 CSeq 以区分重传或迟到响应，保留原文，没有隐藏。

本次锁定 sipgo v1.6.0 的 `sip/transaction_client_tx.go::Init()` 在 UDP 上启动 Timer A，`transaction.go` 默认 T1=500ms，非 INVITE 的 `transaction_client_tx_fsm.go::actResend()` 也会重发。两次 Range 的耗时超过500ms，且其响应所在秒都有未匹配200，符合事务重传的可能性。没有同次网络抓包，不能确认设备实际收到几份、对重复 CSeq 如何处理，也不能区分媒体跳变由首发还是重传触发；不将此次结果描述为无重传实验。

`go test` 的 PASS 只表示取证脚本完成及清理断言通过，不表示四项客户端功能 PASS。私有 overlay 已编译，未运行新的仓库 CTest/Go 全量测试，因为没有生产或正式测试代码变更；此前验证不转写为本轮新 PASS。

Claude Code 对设计进行了只读审查，指出凭据、adapter、结束顺序、录像预算和边界问题；实际探针对已知 SIP 端口、204、owner/token、dialog、established 和关闭顺序作了明确处理。独立代码审查提出的 PT96/首媒体与关闭窗口问题已在运行前修正。

## 7. 证据与结论

DEV：`/var/tmp/qt-dahua-seek-d3d3649-29sCxqpQ/`。

TEST：`/var/tmp/qt-dahua-seek-d3d3649-1dQopxyF/`。

两处保留原始事件、RTP、PS、日志；DEV 另有私有探针源码、overlay、编译记录、Claude 审查、ffprobe/FFmpeg 日志和 `run-01/analysis-final.json`。原始材料非公开，不能未经检查提交。

| 文件 | SHA256 |
| --- | --- |
| 私有探针源码 | `dbe48d688bf89869011415421b5be341e08ceee60aa6e550d5e6ef1abe942722` |
| 运行二进制 | `c344ecf789dd6d021f7b4cc2dc041f3c38e2392b9e730ec5c749a55b6bc33945` |
| `events.jsonl` | `c38456ff3b9c3abd7b9fb9b18e6c7e58ed3539959a365d4248fcc25adf2f9179` |
| `rtp.rfc4571` | `36df984410a7c570bbfe959c83b5ba5d73b2ee5e5ce97b77430f413e987287d3` |
| `media.ps` | `f77d287f13f64b87a4368645b1a00e9cd67a791fcc034465a5fdeb4e5d6d0fea` |
| `analysis-final.json` | `73832d8c74f7bc7cd75a436c8167d26f3a14b645837a8e1fc4a414af8ea2f65f` |

结论：设备在同一对话内实际执行了位置跳变，并在 Scale 请求后改变/恢复发送节奏；本次没有返回 RTP-Info，其他可传到客户端的可靠切换边界也尚未验证。并不是只有 RTP-Info 才可能实现定位，但离线看出 PTS 跳变不等于已经定义了业务边界。**Qt 同会话 seek/倍速仍为 NOT_IMPLEMENTED**，不新增“200 即成功”的假接口，不猜 PTS 边界，不重 INVITE 冒充同会话定位。

下一步必须先确定可验证的媒体边界契约，以及解复用/解码/PCM/呈现的同代刷新和倍速音频策略；在此之前保持现有 pause/resume 和独立回放/下载行为。广播/对讲仍需独立的反向 audio INVITE 与纯 PCMA RTP 路径，现有视频 PS sender 不能代替。完整平台仍未 GO。

## 8. 离线补证：宏块错误归因与倍速音频（2026-10-17）

本节只读复用第7节原始 `media.ps`，未访问相机、重跑网络或修改旧输入、`analysis-final.json`、日志和结论记录。原第5节保留了当时尚不能归因的事实；以下证据补足归因，**不支持将宏块错误认定为 seek 导致**，也不是 Qt 呈现通过。

### 解码与截尾对照

私有 C++ 探针使用非 seekable custom AVIO，比较不刷新与在单轨相邻有效 DTS 差绝对值超过1秒时、送入该 packet 前调用 `avcodec_flush_buffers()`。这个阈值只用于本样本实验，不是通用生产边界算法。先用 `/home/gyl/ffmpeg901` 静态9.0.1，独立复审发现它不是当前 Qt 的依赖构建；旧记录不覆盖，另用 Qt 实际 shared SDK `/var/tmp/qt-broadcast-sdk-755cf52-z2y4wEha/install` 重做四组短对照，保留 `ldd`、逐次 runtime maps 与三库 SHA。两套结果相同，不能因版本同为9.0.1就混称同一依赖。

| 输入与模式 | 视频/音频解码帧 | corrupt packet/frame | 诊断 |
| --- | ---: | ---: | --- |
| 完整原采集，不刷新 | 808 / 538 | 1 / 1 | DTS倒退、尾部损坏及宏块错误 |
| 完整原采集，四次单轨刷新 | 808 / 538 | 1 / 1 | 同上，刷新没有改善 |
| 只读取此前完整pack边界，不刷新 | 807 / 538 | 0 / 0 | 仍保留真实DTS倒退警告 |
| 同一读取限制，四次单轨刷新 | 807 / 538 | 0 / 0 | 同上 |

四组各自退出0不代表全文件无错。原文件长17,137,727字节；最后视频 PES 位于17,126,609，声明结束17,141,839，缺4,112字节。`Packet corrupt dts=2540520` 和宏块错误均关联此尾部；限制 AVIO 读取至此前完整pack边界17,126,595后，两模式corrupt均归零，不是修改原文件或把截尾录像宣称完整。parser 可能延迟标志，不能只按 `AV_PKT_FLAG_CORRUPT` 所附前一输出包的位置误判根因。

真实前跳/回跳首个视频包分别位于2,947,085和5,964,945，PTS/DTS分别为1,824,300和615,510；两处各自的新旧四个包，其大小与payload SHA都与原PS对应PES分片拼接完全一致。新包137,996及78,430字节，未观察到旧/新 PES 混接。回跳处音频先于视频输出，不能见任一轨变化便对所有轨反复清空。FFmpeg源码对应 `libavformat/mpeg.c:483–670` 的PES读取、`utils.c:59–91` 的短读corrupt、`demux.c:587–595` 的警告，源码位于DEV `/home/gyl/ffmpeg-build/ffmpeg-9.0.1/`。

因此本样本没有支持生产 codec flush 的 RED→GREEN。seek 的本地呈现/PCM时钟仍须适配，但不应为解决已归因的采集截尾而增加解复用重建、丢弃原始HTTP字节或通用状态机。缺少 RTP-Info 不阻止 best-effort 展示设备实际送来的画面；它限制的是精确边界归属和命中承诺，不是“定位功能必定不可实现”。INFO完成后首包仍不能当作新位置首包。

### PCMA 在2×时同样加速

从原采集事件、ffprobe包位置与RTP到达映射离线重算，严格窗口为启动后37.719289020至41.719915766秒，截止于下一次1×控制发送前。窗口内123个PCMA包×480样本＝59,040样本，约14,757.6877样本/墙钟秒，音频PTS/墙钟OLS斜率1.8979614。恢复1×窗口约7,966.1266样本/墙钟秒。仅为这次设备录像的短窗观测，不是所有倍率的误差/置信区间或实际声音验收。

最初探索窗止于41.849810989秒，跨过下一次1×发送约130ms；相关127包/60,960样本数字保留在私有补证中，不能把它写成严格隔离窗。上述事实说明不能默认非1×设备不发音频；当前Qt固定48kHz输出全部PCM会把输入背压回近1×。后续必须明确音频呈现策略，不静默丢音后报告双轨通过，不把请求倍率直接当作设备实际倍率。

### 证据、失败与清理

DEV `/tmp/qt-dahua-dts-flush-d983904-tS8akfyD/` 保留源码、重建命令、两套四组 JSONL/stderr、PES逐字节比对、音频算法与输入SHA。shared对照四组及编译各自退出0，但外层脚本最后的元数据哈希命令硬编码了错误库文件名而退出1；失败保留于 `shared-sdk/run-status.json`，另按实际 runtime 路径生成正确清单，未重跑解码或隐藏诊断。

| 文件 | SHA256 |
| --- | --- |
| 原始输入，补证前后不变 | `f77d287f13f64b87a4368645b1a00e9cd67a791fcc034465a5fdeb4e5d6d0fea` |
| 静态构建 `summary.json`（SDK归属勘误另列） | `a8f70044434d5ddc9ef912b150d7485e3463a49a0586217967b2c407d8fba0b4` |
| `shared-sdk/shared-summary.json` | `561249bd5226b50e5fe08188ad04ee604f4c1a0a6931d3cc1d04efc959f8bd09` |
| `audio-rate.json` | `ce9e8f90c2dbc46ff707325169a332184feecec6b95a9e30bbf0a3330dd0a4d7` |

用户要求只清理本轮自有无用中间产物。精确核对后删除旧静态 `probe` ELF，66,000,936字节，原SHA及重建命令保留；完整日志、源码、动态小探针和原media均未删除，详见 `cleanup-owned-static-probe.md`。旧manifest保留历史条目，ELF已清理的事实另记在该清理说明中，不能声称原ELF仍可现场校验。

本节不改生产解码器、不关闭告警、不改变原 pause/resume、有限EOF和授权行为；未运行新的 Qt/SDL、ASan、摄像机或完整媒体回归。Qt 同会话 seek/倍速及完整平台仍未 GO。
