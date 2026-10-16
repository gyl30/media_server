# 阶段五：本地录制与设备录像下载

本增量实现阶段五的本地录制与设备下载子集；抓图在阶段二已有。它不是物理 NVR 或摄像机验收，也不代表下载内容等于请求范围的设备库存。

## 组件与依赖

- `record_thread`：输入实时 RTSP 或有限 PS（HTTP chunked/本地文件），只流复制 H.264 与 AAC/PCMA/PCMU 到 Matroska，不解码、不转码。MP4 不支持 PCMA，因此统一使用 MKV。以 `QFile::NewOnly` 新建 `<目标>.part`，经自定义 AVIO 写入与 seek；`avoid_negative_ts=make_zero` 保留音视频相对偏移，时间戳按输出时基换算。保存必须依次完成 trailer、AVIO 错误检查、刷新、关闭与 rename；收尾后仍检查取消。任何失败只删除本次新建的 `.part`，已存在的目标或同名临时文件都拒绝写入且不改动。
- `finite_http_input` 从解码线程原样抽出为独立文件，解码与录制两处复用；行为不变。
- `history_request` 从 `play_record` 抽出：固定平台地址、校验 201 与 `ps_url`、使用方销毁或失败时只 DELETE 本实例、迟到身份回收；回放与下载两处调用。
- 桌面 FFmpeg shared LGPL SDK 在原选项上只追加 `--enable-muxer=matroska --enable-bsf=extract_extradata`（`aac_adtstoasc` 随 muxer 自动选中）。新前缀与配置：DEV `/var/tmp/qt-claude-owner-work/ffmpeg-mkv/`（`configure-command.txt`、`install.sha256`），源码与正常服务器 SDK 未改。

## 语义

| 场景 | 保存条件 | 不保存 |
| --- | --- | --- |
| 本地录制（实时） | 已写入视频后用户停止、画面停止或替换、源结束或网络中断；后两者提示“中断” | 取消、写盘失败、收到视频前停止 |
| 设备下载（有限） | HTTP chunked 解析完整且解复用正常 EOF | 取消、关闭录像窗口、缺终止块、损坏包、探测后出现未识别的新音轨、写盘失败 |

实时录制从首个视频关键帧写起，之前的音视频包都不写入；有限下载按原样完整复制。录制不使用实时显示的 NOBUFFER，避免丢掉首个关键帧。本地录制另签独立播放票据，与观看共享上游、观看身份独立；画面停止后迟到的票据不会启动录制。退出程序时 `stop_all()` 让录制收尾、关闭录像窗口取消下载，主窗口异步等待所有录制线程退出后才关闭。

## 设备下载信令

`POST /api/devices/{d}/channels/{c}/download {start_time,end_time,speed}` 复用历史 owner、票据、GET 认领后 INVITE、121/BYE/DELETE 与资源回收，不新增会话字段。SDP 使用 `s=Download` 与 `a=downloadspeed:N`，`N` 只接受 1/2/4；设备是否支持以实际应答为准，不自动降速重试。SIP 会话用途由新的 `gbSession`（实时/回放/下载）表达，接收器路径与 SSRC 首位仍由 `receiverKind` 决定。绝对预算为 `ceil(时长/N) + 票据 TTL + 60 秒`，先按整数秒向上取整并比较可表示上限，再转 `time.Duration`。回放请求出现 `speed`（含 0 与 null）即拒绝；下载缺失、null、字符串或其他倍速均拒绝。

## 界面

录像查询窗口新增保存目录（与本地录制共用设置，默认系统影片目录）、倍速、下载/取消与进度；文件名为 `<通道>_<开始>_<结束>.mkv`（设备当地时间）。一次一个下载，结束后只回收本次实例。主窗口底部“开始录制/停止录制”作用于选中的实时画面，文件名为 `<名称>_<时间>.mkv`，名称中字母数字与 `-` 以外的字符替换为 `_` 并截断，不会写到所选目录之外；画面右键可设置录像目录。

已知边界：取消下载后按钮立即恢复，若在旧线程删除临时文件前重下同一录像，新任务会因 `.part` 已存在而失败并提示，需稍后重试；目录选择对话框未纳入自动化测试。

## 验证

证据目录均在 DEV `/var/tmp/qt-claude-owner-work/`。

- `record_thread_test` 15 项（`stage5-qt/record-02.log`、`asan-record-02.log`）：2 秒 PCMA/AAC 与 8 秒 PS 输出 MKV 再以同 SDK 解码，逐帧与 PCM 和源一致，首视频时间戳 ≤50 ms、时长误差 ≤100 ms；HTTP 完整/缺终止块/取消/完整分帧但末 PES 截断；目标或 `.part` 已存在；`RLIMIT_FSIZE` 在正文与收尾阶段失败；实时模式用户停止与源结束。实时模式用 FFmpeg 可中断的 http 协议代替单元测试中不可得的 RTSP 服务端。
- 保留的失败：`record-01` 有限模式误做关键帧门控丢掉关键帧前读到的音频包；Codex 指出的映射越界、收尾取消、损坏包三项，其中截断 PES 旧代码保存（`red-02-*`），探测后新音轨用 FFmpeg mpeg muxer 生成的无 PSM 素材 `late-mp2-audio.ps` 复现为静默丢音频并保存（`red-05-late_stream.log`），未复现 ASan 越界；ireader 生成的 PS 带 PSM，探测时即建立音轨，不能复现。
- `gb_download_test.go`：SDP、预算向上取整（含 2^62 量级）、参数拒绝、预算约 3 秒 + TTL + 60 秒、认领前不 INVITE、真实 UDP SIP 下 INVITE 体含 `s=Download`/`a=downloadspeed:4`/原始时间范围、DELETE 后资源归零。
- `record_query_dialog_test` 下载 4 行（完成写出 EBML、取消、关窗、创建失败带 `playback_id` 回收）；`playback_view_test` 本地录制 2 行（独立签票、收到视频前停止不留文件、画面停止后迟到票据不启动录制）。
- 全量（`stage5-final/`，新 SDK、素材目录 `fixtures-v2`）：桌面 normal 与 ASan+UBSan 各 7/7 CTest、393 项 QTest，无失败/跳过，204.01/206.28 秒，未检出 sanitizer 报告；Go `go-full-02.log`（111.726 秒）、`go-race.log`（116.904 秒）、`go-vet.log` 通过。首轮 `go-full.log` 的 `TestPresetControlFailuresStillStop/timeout` 失败签名与 HEAD 既有不稳定相同，保留未改。期间只修改了非编译输入的 README。
- 真实下载组合 `stage5-integration/download-run-02*`：实际 Go HTTP/票据/认领、受控 UDP SIP 设备、冻结的 C++ 有限通道 ELF，私有探针调用生产 `request_history` 与 `record_thread`（不是窗口点击）。normal/ASan × PCMA 8 秒/AAC 2 秒/中途取消六组通过：下载文件 200/200、50/50 帧逐帧与参考一致、PCM 一致；取消不留文件；INFO 0、创建/DELETE/INVITE 各 1，DELETE 触发的 BYE 为 INVITE+1；owner、票据、SSRC、接收器归零。`download-run-01` 因夹具仍按 `/playback` 计数失败，保留。
- 真实本地录制组合 `stage5-integration/live-run-03`、`live-run-04`：当前信令、冻结媒体 ELF、ffmpeg 以 RTMP（AAC）或 RTSP（PCMA）推流，私有探针用实际 `playback_view` 观看后录制约 6 秒。normal/ASan × AAC/PCMA × 用户停止/源结束八组通过：152–155 帧、时长与帧数一致、音频可解码，源结束提示中断仍保存；`live-run-04` 在修正中断文案后复跑 PCMA 源结束两组。`live-run-01` 因探针参数与 Qt `-platform` 冲突失败，保留。

不在范围：物理 NVR 下载、设备倍速与断点续传兼容性、超大文件与长时录制、磁盘满（以 `RLIMIT_FSIZE` 代替）、录像窗口目录对话框的自动化。
