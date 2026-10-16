# 大华历史录像延迟收尾与 Qt 有限传输期限

本次接续 `dcb4fd6`，只修正 Qt 历史回放及下载误用实时进展超时的问题；不修改相机录像、服务端预算、SIP 收尾或媒体服务器，不代表阶段四至七或完整 GB 平台 GO。

## 真实根因与 RED

真实大华通道 `34020000001310000089` 的既有库存中，请求 2026-10-16 07:11:50–07:12:00（UTC+8）。未修改录像计划、编码或库存，也未删录像。GB RecordInfo 在相应库存窗口返回一条记录。

同一次观测中，相机最后 TCP 有效负载为 09:18:32.421451 UTC，FIN 为 09:18:52.300012；同主机单调时间差 **19.878583651 秒**。HTTP 末正文至完整 EOF 为 **19.878084220 秒**。此前已收到 MediaStatus/121 并完成 BYE/200，不能将 121 直接冒充媒体 EOF，也不能据本设备一次行为硬编码新的 20/30 秒媒体超时。

完整 PS 为 5,134,937 字节，SHA256 `c9ae39899360199b675ac0417ffbba61c5ba4564b6edd9d2f2c11f7323c15402`。独立 FFmpeg 解码退出 0，H.264 Main 1920×1080/25fps、PCMA 8kHz mono，共 247 视频帧；解码日志保留两条 null muxer DTS 非递增警告，因此不宣称时间戳完全无异常。HTTP 正常结束也不证明请求时间段库存完整。

旧 Qt 真实回放在第 246 帧后误报“视频解码无进展（10 秒）”，PCM 为 475,104 样本，而完整参考为 475,200 样本；旧下载同样超时，无最终文件且 `.part` 被清理。原始失败保留，不改写成 PASS。

## 最小修正

- `history_request` 在唯一的 201 校验处读取既有 `deadline`，要求合法、显式时区且未来；仅转换一次为单调 `QDeadlineTimer`，按值交给现有解码/录制线程及局部 HTTP 输入。
- `expires_at` 是一次性 URL 的认领期限，不是传输期限。历史 owner 的绝对预算从平台创建时开始，现有公式为请求时长除速度向上取整，加 token TTL 及 60 秒，见 `signaling/gb_playback.go`；本轮不修改或续期。
- 连接、TLS 与 GET 仍受原 15 秒和绝对期限的较早者限制；有限 HTTP 打开成功后，FFmpeg 打开、轨道探测、正文和尾部等待改用原绝对预算，避免短 PS 探测再次被 15 秒截断。
- HTTP 未完整时，有限视频/PCM 队列背压也检查同一期限，暂停不续租；缓存包不能绕开检查。缺少结束块、截断和读错误仍失败，不伪造 EOF。
- HTTP parser 已完整后，不用网络期限限制本地解码 flush、重采样、用户暂停排空和文件收尾；取消始终有效。
- 实时播放/录制保留音视频各 10 秒进展约束。删除解码线程的暂停采样位、进展时基字段、`set_paused()` 和背压补时逻辑；UI 暂停事实仍保留。新增的仅是已有平台期限的值类型移交，无 timer、registry、关闭标志或第二套状态机。
- 下载继续使用原 `.part` → 完整收尾 → 改名，失败/取消不提交文件；原历史 DELETE 补偿和异步所有权不变。

## 自动回归与复审

稳定 RED：8 秒 PS 正文之后延迟 11 秒结束，旧回放/下载均超时；2 秒短 PS 延迟 16.5 秒结束，旧轨道探测超时。新回归严格检查完整帧/PCM、缺 EOF 预算、视频及音频满队列预算、完整 HTTP 后预算已到期仍可排空。

首次预算测试误用短 PS，仍阻塞探测、没有进入预期解码路径，三行断言真实失败；保留该日志，改用已有 8 秒 PS，未放宽断言。票据覆盖缺失/错误类型/非法/无时区/过期拒绝及 DELETE、零媒体 GET；合法 Go 九位纳秒 Z 和显式 UTC+8 通过。自动化有限视图暂停场景另检查完整 HTTP 后期限届满，恢复仍严格呈现 50/50 帧。

Normal 桌面完整构建和 CTest **8/8，301.29 秒**；LastTest 中 **417 项 QTest、零失败/跳过**。解码 CTest 超时 45→90 秒仅容纳新增 11 秒及 16.5 秒真实故障条件，不改变媒体健康标准。

ASan+UBSan 完整桌面 CTest **8/8，169.61 秒**（`-j2`），同样 **417 项 QTest、零失败/跳过**；保留 `detect_leaks=1:halt_on_error=1`，无 sanitizer 诊断。Normal/ASan 耗时不同不能当作性能对比。Claude 设计/当前代码只读审查及独立 Codex 复审均未发现 P0/P1；讨论不代替测试。

## 真实相机 GREEN

沿用同一库存范围和原 PS 参考，不改变探针的成功断言，只让下载探针将新期限参数传给生产线程：

| 路径 | 实际结果 | 清理 |
| --- | --- | --- |
| Normal 有限视图 | 退出 0；30,337ms；247 帧全部解码/逐帧 Paint 匹配；475,200 PCM 样本；双轨零丢弃 | DELETE 204，decoder/operation 最终析构，SDL 队列排空 |
| ASan+UBSan 有限视图 | 退出 0；32,826ms；相同 247 帧及 PCM 计数、逐帧一致 | 同上；无 sanitizer 报告 |
| Normal 下载 | 退出 0；247 视频包、165 音频包；保存 MKV 后重新实际解码，247 帧及 1,900,800 PCM 字节与参考一致 | 最终文件存在、无 `.part`，DELETE 排空 |
| ASan+UBSan 下载 | 退出 0；同样完整包/帧/PCM，PCM SHA256 `51eaf2173dc03f7f4e1da687b651c23fed24832d84ff17aed87fa1f6a1923135` | 同上；无 sanitizer 报告 |
| Normal 暂停/继续 | 退出 0；33,449ms；暂停确认后呈现冻结于第46帧，3秒后请求继续；最终247帧逐帧匹配、475,200 PCM样本 | DELETE 204，decoder/operation 最终析构 |

视图探针从生产公开路径观察解码计数与 Paint 图像；PCM 哈希是参考解码及下载文件重解码的证据，不把网络视图计数冒充逐样本 hash，也不把 offscreen Paint 当作物理显示器。

真实暂停复测仅证明平台请求受理与本地冻结/继续及内容完整，未在同一次暂停窗口抓取上游媒体元数据，不能声称设备发送确实完全停止。该真实暂停场景没有另跑 ASan；ASan 自动化暂停/背压由完整桌面套件覆盖。

## 证据索引与边界

DEV 私有目录 `/var/tmp/qt-dahua-controls-md9w0l/`；TEST `/var/tmp/qt-dahua-a2fcc80-V2mC1Ksf/run-06-controls/`。原始结果可能含票据或环境信息，不提交。

- `history-wire-analysis.json`：同次 SIP/TCP/HTTP 时序；`history-wire-timing.jsonl` 为 7,429 条解析元数据，不保存原始报文。网络 payload 计数不是去重的应用字节。
- `history-http-timing-03/`：原 HTTP、PS、CLI 解码和清理。
- `qt-history-normal-01/`、`qt-download-normal-01/`：真实设备旧版 RED。
- `history-tail-*-red.log`、`history-probe-decode-red.log`：稳定自动 RED。
- `history-tail-decode-green.log`：首轮三项夹具路径失败；`history-budget-green02.log` 和 `history-past-eof-green02.log` 为修正后的定向 GREEN。
- `history-full-normal.log`、`history-normal-LastTest.log`：完整 Normal 回归。
- `history-full-asan.log`、`history-asan-LastTest.log`：完整 ASan+UBSan 回归。
- `qt-history-{normal,asan}-green01/`、`qt-download-{normal,asan}-green01/`：真实相机 GREEN、原日志与机器可读结果。
- `qt-history-pause-normal-green01/`：真实暂停/继续与上述上游验证边界。
- `history-probes-green01/`：精确探针源码、构建脚本及二进制 SHA；原 `history-probes/` 和旧失败未覆盖。
- `history-real-green-commands.txt`：本轮实际参数与退出码摘录；不是再次执行脚本，补充结果 JSON 未保存请求范围的边界。
- `history-validated-source.diff`：已验证源码差异，SHA256 `31187942b0493a56bd7221f2bcc7df13ea47d57be4d262694a6abd97eedcb419`。

客户端和平台需要合理校时；收到响应时只进行一次墙钟→单调转换，没有跨机偏差估计。静默黑洞可能等到整个 owner 预算（长下载可较长），用户可随时取消；没有另加 idle timer。HTTP 尚未完整的长暂停会消耗原预算，完整后本地呈现超过期限不等于继续持有服务器资源。

offscreen、SDL dummy、数字音频回环和物理桌面/扬声器分别验收；本次不能将 dummy 排空当作物理音频 PASS。SDK、Qt、SDL 等外部库并非全部 sanitizer 插桩。本阶段不改 Go/C++ 服务端，不声明重跑了其全量测试。普通回放成功不能外推 seek、倍速、其他 NVR、广播/对讲或 GB 全标准符合性。

## 本轮环境收尾

最后 `/api/sources` 返回空列表；自有 signaling/media_server 均正常退出 0，按 PID 身份复核已消失，独立监听端口释放；自有 SSH 转发核对完整命令与 starttime 后停止。仅删除相同 SHA 的重复源码差异快照及可重建的 Qt 日期探针二进制，保留源码、日志、原始 RED/GREEN、媒体及 SHA。相机 GB 平台保持指向 TEST；本次录像验证没有改相机配置或删除库存，前一云台阶段已归位并删除临时预置位。
