# Qt 有限录像呈现增量

## 范围与状态

本增量属于阶段四，不代表完整录像业务或整个 GB 平台完成。客户端继续只有一套 FFmpeg 解码路径，复用 Qt Widgets 和 SDL2；未引入 VLC、另一套 SIP 平台、实时媒体历史缓存或新的媒体分发层。

录像目录中的设备墙上时间，经用户确认的固定 UTC 偏移换算为 Unix 秒，再申请独立回放。目录的 `file_path` 只作展示，不能当作可播放 URL。当前不自动推断设备时区或历史夏令时，偏移默认值来自本机当前值，操作员必须核对。

当前尚未实现同会话 INFO 暂停、恢复或 seek。不能把重新签票、重开范围或本地停止呈现称为协议暂停/seek。下载、真实 NVR 库存完整性和物理音视频设备验收也不在本增量的 PASS 范围内。

## 所有权与有限缓冲

- `playback_view::play_record()` 为本次操作固定平台地址，创建独立请求对象；原平台地址在主界面被修改后，旧回放的 DELETE 仍发回原实例。
- 创建请求有 20 秒期限。关闭画面不丢弃尚未返回的创建身份：迟到的 201 或携带清理身份的失败响应仅回收本次历史实例，不打开媒体、不停止共享实时上游。
- 有效响应创建 `playback_mode::finite` 解码线程。画面停止只请求中断并保活到线程结束，不在 GUI 等待网络线程。
- 解码视频队列最多 4 帧，PCM 最多 57,600 字节；有限模式满队列等待消费者，取消可解除等待，不沿用实时模式的丢旧帧/裁 PCM 策略。
- GUI 视频队列最多 16 帧，只有有足够空间时才取一批解码帧；PCM 本地队列排空后才取下一批。SDL 排队阈值不是硬性最大值：追加一个完整块后可高于阈值，但仍受完整块及上游队列边界约束。
- 有限视频每次 GUI 更新最多呈现一帧，不为了追赶时钟丢帧。正常结束必须先观察生产线程已结束，再取完末批队列，等待最后视频帧时长和音频队列尾部，而不是把线程结束当成呈现结束。
- 真实停止和错误仍允许丢弃剩余内容；它们不能标为正常播放完整结束。

新增 `playback_mode` 表达有限输入与实时输入的真实差异；队列条件变量用于生产者背压；最后显示帧的结束 PTS 用于该帧已移出队列后的尾部判断。没有新增 started/closed 标志、Token 回滚或通用播放器状态机。

## HTTP 结束必须由协议解析确认

DEV 的 Qt 6.2.4 独立复现表明：缺少 chunked 最后零块时，Qt 网络层有时报告 `NoError`，有时在收到全部正文后仍不结束。单凭 Qt finished/error 不能证明有限录像传输完整。原始 RED 和两种独立网络复现均保留：

`/var/tmp/media-server-qt-finite-view-2UTx0U/http-diagnostic-0T4gcq/`

有限 HTTP 输入因此使用现有 Boost.Beast `response_parser<buffer_body>` 解析分帧，Qt socket 提供 TCP/TLS；并不实现第二个媒体播放器。只接受本平台合同的 200、`video/mpeg` 和 chunked 响应，无重试或重定向。只有 parser 完整结束允许 EOF；先消费 Qt 已缓冲字节，再将 FIN 交给 parser，缺块、短正文或非法分帧必须报错。正常 HTTPS 保留证书与主机名验证，没有 `ignoreSslErrors()`。

接收缓冲和 HTTP 头均有界；总正文长度不设固定录像时长上限，不意味着内存无界。网络操作在解码线程执行，沿用连接和轨道进展期限，取消等待最多按 50 毫秒检查一次。Boost 只是现有头文件依赖，不需要桌面客户端链接 Boost.Context。

正常结束文案为“录像传输及呈现已结束；不代表请求范围库存完整”。TCP 边界、HTTP 完整、FFmpeg 解码完整和设备库存完整是四种不同事实。

## 目录选择与时间回归

Qt 的 current item 与 selected item 不等价。旧实现在 `clearSelection()` 后仍可使用旧焦点行，真实 RED 观察到一次错误回放信号。修复只在原三个检查处要求该行仍被选择，未新增选择状态。

证据：`/var/tmp/media-server-qt-record-playback-4yeYxK/`

- `clear-selection-red-01.log`：旧实现失败，未改断言。
- `clear-selection-green-01.log`：同一用例通过。
- `full-green-01.log`：133 项 QTest 通过，无失败或跳过。
- 新增固定 UTC 偏移、非法/非规范时间、清空选择和查询中禁用回放回归；原目录查询语义不变。

## 解码与呈现证据

主证据目录：`/var/tmp/media-server-qt-finite-view-2UTx0U/`。使用独立、包含 MPEG-PS demuxer 的 FFmpeg SDK：`/var/tmp/media-server-qt-finite-ps-kLBXkM/install`；未改变正常服务器 SDK。

- `decode-directed-01.log`：保留首轮两个失败，不视为 PASS。
- `decode-directed-02.log`：20 项通过、零跳过，覆盖完整 BGRA/PCM、有限背压取消、PS、HTTP 缺零块及既有实时取消/超时路径。
- `finite-view-01.log`：保留测试对象识别错误。`platform_client` 没有独立 Qt meta-object，不能用 `findChildren<platform_client*>` 将所有 QObject 子对象当作该类；现改为实际 C++ 类型检查及 `QPointer` 析构观察，不改生产类来迎合测试。
- `finite-view-02.log`：15 项通过、零跳过。覆盖取消/析构后的迟到 201、平台切换、挂起 GET 取消、失败补偿，以及 AAC/PCMA × 视频先/音频先 × 静音/SDL dummy 八组 PS；每组实际 Paint 事件使用的全部 50 帧 BGRA 与同 SDK 本地参考逐帧一致，独立请求对象和解码线程最终释放。

这些 Paint 事件不是物理显示器扫描输出；SDL dummy 队列检查不是数字音频回环，更不是扬声器声学验收。同 SDK 参考用于传输及呈现完整度，不能扩大成独立编解码正确性证明。

AAC MP4 参考存在 SDK/CLI 版本差异，独立证据保存在 `/var/tmp/media-server-qt-aac-tail-bMv3Pj/`：合法输入时长 48,000 样本，旧 CLI 额外输出 128 个 padding 样本；裁到明确的合法时长后，全部 48,000 样本 PCM 与 SDK 完整 SHA256 相同。测试仅规范化 AAC 参考的合法时长，未放宽样本、字节或完整哈希断言。

后续 HTTP 回归扩展到八种分帧/状态场景：完整、真实分片、缺零块、非法长度、短正文、非 200、错误类型、结束块后额外字节。首次分片夹具人为以数百次 timer 事件限速，超过原四秒断言；原因和原失败保留，修正只减少实际写入次数，仍拆开状态行及 chunk 边界，不修改四秒或完整媒体断言。

主窗口追加接线回归之前的桌面全套检查点（非最终源码）：

| 配置 | 证据日志 | 结果 |
| --- | --- | --- |
| RelWithDebInfo | `normal-ctest-02.log` | 6/6 CTest；360 项 QTest，无失败/跳过；133.28 秒 |
| ASan+UBSan | `asan-ctest-03.log` | 6/6 CTest；360 项 QTest，无失败/跳过；135.28 秒 |

Sanitizer 保持 `detect_leaks=1`、发现错误终止；桌面第一方实现有插桩，系统 Qt、SDL、独立 FFmpeg SDK 不因此被宣称全部插桩。更早的 `normal-ctest-01.log` 与 `asan-ctest-02.log` 的夹具超时失败仍保留，不覆盖成 PASS。

真实组合证据位于 `history-gui-integration-01/`，`run-01-path.txt` 指向本次独立结果目录。私有探针链接实际 `desktop_media`，由 `play_record()` 唯一创建请求，非 mock ticket 或私有替代 decoder；Go 真实 HTTP handler、受控 SIP UDP peer、生产 C++ RFC4571 接收、正常 HTTP chunked、Qt 呈现和 DELETE 串联：normal/ASan × AAC/PCMA 四组通过，Go 总计 10.958 秒。每组创建/GET 认领/INVITE/ACK/BYE/DELETE 各一次，50 帧实际 Paint 图像与参考全序列一致，AAC 原始及 PCM 均 97,280 样本，PCMA 原始 16,000、重采样 PCM 96,000；结束后 owner、票据、SSRC、receiver 均归零。

组合测试使用 offscreen/静音和预置注册事实，不声称真实 REGISTER、物理 NVR、显示器或声学播放。公开 view API 只暴露网络音频解码/重采样计数，不暴露 PCM 块，故本次 GUI 组合不编造网络 PCM hash；此前独立传输组合 `/var/tmp/media-server-qt-history-media-L8h9VQig/run-05.log` 的精确 PCM 证据仍独立保留。

SIP 测试夹具关闭时的 `UDP ref went negative ... ref=-1` 告警原样保留，旧 run05 与本轮四组都有相同现象。锁定 sipgo v1.6.0 的 `transport_udp.go` 中 listener 不走此告警；非 listener 连接存在 pool 强制 Close 归零后，读循环 defer 再次 TryClose 的源码路径。这与既有 UA 拆卸时序相符，但没有动态调用栈，不宣称精确根因已经完全证明，也不保证生产退出绝不会遇到。它不是本轮新增 GUI 缺陷的证据，也不能当作 sanitizer 错误或悄悄删除。

## 私有数字音频回环补测

使用独立 Pulse daemon 和唯一 48kHz/S16LE/stereo null sink，未连接硬件音频设备。SDL 实际 driver 为 `pulseaudio`；同一真实 Go/SIP/C++/Qt 路径各播放一次 PCMA、AAC，由 monitor 捕获数字输出。同 SDK 本地有限解码原样保存完整参考 PCM，不调整增益、不重采样、不删首尾样本；分析要求整个参考字节序列连续、四字节对齐，前后只能是全零静音。

| 输入 | 完整匹配样本/声道 | 参考字节 | 完整参考 SHA256 |
| --- | --- | --- | --- |
| PCMA | 96,000 | 384,000 | `425362a2f2398deab2aa845be17784c53a1c94d33f42b1bbc290ac10999ee75d` |
| AAC | 97,280 | 389,120 | `211971576d4903b3453a7ab23b52f424065e12901164d8b5b02d228b736f318c` |

两组精确匹配通过，原始录音、独立 daemon 配置、命令及分析保存在 `/var/tmp/qt-history-pulse-wrapper-7xVslv/{pcma-01,aac-01}/`，`pcm-analysis-01.json` 是音频内容结论；wrapper 的 `result.json` 只确认运行隔离和清理，不能单独作为音频 PASS。参考探针及严格分析源码位于 `/var/tmp/qt-finite-view-pcm-v2-rQd5Cn/`，原私有探针和原始结果未覆盖。PCMA 参考位于 `/var/tmp/qt-hist-ref-pcma-oVoEStD2/`，AAC 本次组合位于 `/var/tmp/qt-hist-aac-4ma4LOeo/`。

daemon、parec、runner 均退出 0 并回收，前后桌面原有 Pulse PID/starttime、socket 和默认设备一致。本补测证明数字 PCM 包括尾部完整送达，不证明扬声器声学效果、物理画面扫描或精确唇音同步。

## 主窗口排队操作隔离

追加主窗口接线测试发现真实 RED：刷新设备列表、切换平台、关闭查询窗口或重新打开查询窗口后，旧窗口排队的回放 click 仍能发出一次 POST；换平台场景误发给新平台。原失败日志保存在 `/var/tmp/media-server-qt-monitor-record-lQ0LcH/`，不以测试模拟 SIP 或物理摄像机。

最小修复把回放连接绑定本次 `listing_`，并检查发送窗口仍是当前 `record_dialog_` 且可见。使用既有 QObject 生命周期和窗口 identity，不增加关闭字段。四个场景的 RED（`queued-reload-red01/02`、`dialog-close-reopen-red01`）、normal 六行三轮及 ASan 六行定向 GREEN 保存在同一目录。

## 零字节响应后断开

提交前本机 Codex 只读复审发现：GET 已发出、服务端未返回任何字节即 FIN 时，有限输入调用 Beast `put_eof()`，违反其 `got_some()` 前置断言。未定义 `NDEBUG` 的私有 Debug 构建新增 `finite_http(FIN-before-response)` 后真实 RED，进程 SIGABRT 退出 134；正常与 ASan 构建均带 `-DNDEBUG`，原全套不能发现。修复仅在 `put_eof()` 前对未收到任何响应字节返回 EIO，不改变其他分帧判定。同一 Debug 构建三轮 11/11 GREEN。证据：DEV `/var/tmp/qt-claude-owner-work/put-eof-assert/`（`red-01.*`、`green-0{1,2,3}.*` 及源码 SHA）。

## 最终桌面全套

主窗口修复与零字节响应修复之后，同一源码清单（`final-05.source-SHA256SUMS`，SHA `008b808073320b4bbd2963238287ce4477111b62ecceab68cfd5e7d6befa10d5`）运行前后不变：

| 配置 | 证据日志 | 结果 |
| --- | --- | --- |
| RelWithDebInfo | `normal-ctest-05.log`、`.LastTest.log` | 6/6 CTest；367 项 QTest，无失败/跳过；134.94 秒 |
| ASan+UBSan | `asan-ctest-05.log`、`.LastTest.log` | 6/6 CTest；367 项 QTest，无失败/跳过；137.37 秒；未检出 sanitizer 报告 |

零字节修复前、主窗口修复后的中间检查点 `normal-ctest-03.log`、`asan-ctest-04.log` 均为 6/6、366 项（135.67/136.80 秒），保留但不代表最终源码。

## 已知边界

- 正常关闭画面会 DELETE 本次实例；进程退出时 DELETE 只尽力发送，主窗口不等待其完成。残留实例依赖服务端未认领 TTL、媒体对账或绝对 deadline 回收，删除失败可能进入 CleanupPending。
- 音视频首 PTS 偏移较大时，早于音频时钟的视频帧按每次更新一帧加速追赶；不主动丢帧，但该段节奏与唇音同步不受保证。

## 提交前复审

原 Claude 第 51/52 轮因额度限制未完成，不作为批准。本机 Codex 0.162.1 只读复审三轮：首轮无 P0/P1；追问 Beast 分帧、背压持锁、`deleteLater` 重复、实例回收与 PTS 偏移时发现上述零字节 P0；修复后复审无 P0/P1，并同意以上两项边界按文档说明处理。原始问答在 DEV `/var/tmp/qt-claude-owner-work/codex-review/qt-round{1,2,3}.*`；讨论不替代测试。

INFO 控制、物理设备或整个阶段四仍未通过，不能由上述结果推导。
