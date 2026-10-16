# 真实大华多画面回归与呈现饥饿修复

## 范围与基线

2026-10-16，在 `feature/qt-monitor-client` 的 `a2fcc80d525b138a9275c902c469cd6d807ea77c` 上增量验证。生产变化仅限桌面呈现调度，另增加对应测试、构建注册和验证文档；不修改媒体服务器、Go signaling、第三方或 main。

DEV 为 i7-13700KF、GCC 16.0.1 experimental、Qt 6.2.4、独立 FFmpeg decoder SDK。真实摄像机为 Dahua DH-SD-P3A1205-P，主流配置 H.264 1920×1080/25fps、CBR 4096Kb/s，音频 G.711A/8000Hz/单声道。媒体经摄像机 GB PS/RTP → TEST media_server → DEV 独立 RTSP/TCP 解码。桌面使用独立 Xvfb/XCB；不等同物理屏幕或扬声器人工验收。

测试媒体 ELF 沿用已冻结版本，SHA256 为 `9abe7ac69495ed25a8946182db9bbfa6671edd15fe4f809da151945c48526fe2`；signaling 由 clean `a2fcc80` 构建，SHA256 为 `a3be2ac2a6ac53a7146135cd588c14e3f0752a431e0aaf7992f19fb6ee3d9428`。本轮正常/ASan 桌面 probe SHA 分别为 `9ba6b09d9fd18126cd90c7b6119cacd2bee29100a2e077c7dbb067781a9ddd1f`、`802fefc448f62cad6aabefb6ac12c98524ded8d27c28dddb3b3dff0eee97b2ef`。

## 原始实机操作与结果

用户明确授权后，读取摄像机实际设置，将原平台 `.30:15060` 改为专用 TEST `.32:15060`，设置本轮专用 SIP 密码；未改相机 IP、Web 管理密码、视频和音频参数。Web 密码控件最大长度为 24，最初生成的 32 字符 SIP 凭据被截断，产生 REGISTER/401 循环；对齐平台凭据后注册成功。原掩码不作为真实密码使用。凭据仅保存在仓库外权限 0600 文件中。

真实 REGISTER、Catalog、持续 Keepalive、DeviceInfo、DeviceStatus 和 PresetQuery 均获得实际响应。录像目录查询的指定 14 分钟窗口返回零条，不能推出设备不支持录像。预置位仅查询，未执行物理运动、改名、保存或删除；回放、下载、广播、对讲不在本次实机 PASS 范围。

最初单画面观察 10001ms：解码 250 帧、呈现 249 帧、重采样 PCM 480960 样本，54 项断言通过。最初四画面失败：共同窗口仅 3001ms，一路解码 75 帧但只呈现 8 帧，另外三路各呈现 75 帧。未完成计划时长的结果明确保留为 FAIL。

这轮操作与原始失败保存在 DEV `/var/tmp/qt-dahua-aa4f3f8-tbkLnwXM/`，汇总 `acceptance-result.json`；其 SHA256 为 `c44237d24a37a6d6f51db16dbbb2e927181b45ef836899abd9398bec9429b6bb`。

## 根因与最小修复

`desktop/playback_view.cpp::update_media()` 的实时 GUI 视频队列，静音时最多四帧，选中声音时最多十六帧并限制 PTS 跨度为 500ms。溢出时淘汰最旧帧，但原墙钟仍从首次 PTS 锚点推进；如果保留的队首持续领先时钟 15ms 以上，呈现循环一直等待，接近到期的帧却被后续帧挤掉。这可导致解码持续、呈现饥饿。

私有诊断构建记录 `clock/front/back/shown` 证实实际可达：16 路失败日志中一画面 clock=9170940us、front=9230000us、back=9350000us，而 shown=8070011us；相邻 tick 中队首随输入推进，呈现停滞。该组共同窗口 7101ms，每路解码增加 179 帧，多个画面只呈现 25–36 帧。另一组四路 20 秒原版复测通过，故保留触发波动，不声称每次都会失败。诊断日志不会留在生产代码。

修复只用本次调用的局部 `video_evicted` 记录容量/跨度淘汰；计算本次时钟后，只有实际淘汰、没有音频主时钟且队首仍超前时，才用现存队首重锚已有 `wall_pts_`/`wall_clock_`，并同步本次 clock。未新增成员、重试、缓冲容量、线程、timer 或状态机。正常提前帧仍等待；有声且音频 PTS 已知时不重锚；finite 不淘汰，回放尾部、暂停与节奏不变。

## 受控 RED/GREEN

新增 `desktop/tests/live_playback_view_test.cpp`，通过公共 `play()` → 受控 HTTP 签票 → RTSP/TCP → 真实 H.264 解码验证，不访问私有成员、不新增生产测试接口。FFmpeg 非交互生成 64×64 单 IDR，fixture 每 40ms 发真实 RTP，建立呈现后改变 RTP 时间戳；断言实际解码 PTS 阶跃、至少一秒共同观察、解码和呈现独立增长，以及关闭后 decoder 析构。这里是客户端真实协议回归，不冒充真实 signaling 或摄像机。

| 输入 | 旧版观察增量：解码/呈现 | 修复后：解码/呈现 |
| --- | --- | --- |
| 静音连续 25fps | 30 / 31 | 30 / 31 |
| 静音前跳 500ms | 30 / 0，FAIL | 30 / 31 |
| 声音选中但无音轨，连续 | 31 / 30 | 30 / 30 |
| 声音选中但无音轨，前跳 1000ms | 30 / 0，FAIL | 30 / 31 |

表中为独立采样窗口，边界已有待呈现帧可使呈现增量比解码增量多一帧；不是重复解码。保留 `red-test-01.log`、`red-test-02.log` 与 `green-test-01.log`。找不到 FFmpeg 会明确 QSKIP，生成/编码失败则 FAIL；本轮没有跳过。

## 修复后实机验收

沿用原 probe 的逐画面视频、音频、PTS、呈现和至少 95% 呈现检查，没有放宽断言。

| 配置 | 共同窗口 | 每路视频解码增量 | 每路呈现增量 | 每路 PCM 样本增量 | 断言 |
| --- | --- | --- | --- | --- | --- |
| normal 四路 | 30001ms | 750 | 738–741 | 1440000 | 513/513 |
| normal 十六路 | 30000ms | 747–749 | 745–749 | 1437120–1440000 | 2013/2013 |
| ASan+UBSan 四路，首路声音启用 | 30000ms | 750 | 747–749 | 1440000 | 527/527 |

十六路来自同一真实相机/同一平台 live，不代表十六个不同相机。所有画面各自执行 RTSP/FFmpeg 软件解码。四路和十六路均在完整共同窗口后停止其余 viewer，保留一路再观察三秒；十六路存活画面再解码并呈现各 75 帧，其余十五个 decoder 已销毁。最后关闭全部画面，正常四路/十六路及 sanitizer 四路均正常退出，最终线程回收无超时。

ASan 四路使用 SDL dummy，验证真实 PCMA→PCM 解码、SDL 输出路径和资源安全，不是物理声音、数字声学同步或人为听音验收。Qt、SDL、外部 FFmpeg SDK 和系统库不因应用插桩而自动成为完整插桩依赖。

## 构建、回归与审查

- 完整桌面 normal 构建，`-O2 -g -DNDEBUG`、`-Wall -Wextra -Werror`。
- normal 完整 CTest：8/8，402 项 QTest，0 失败/跳过，123.19 秒（`-j2`）。包括有限回放/暂停、录制/下载、UI、请求取消和新增实时呈现回归。
- ASan+UBSan 完整构建；完整 CTest 8/8、402 项 QTest，0 失败/跳过，123.78 秒（`-j2`）。
- sanitizer 运行开启 `detect_leaks=1`、ASan/UBSan halt-on-error，不使用 suppression；真实四路 sanitizer 测试退出 0，无诊断。
- 此次未改 Go 或媒体服务器；不把既有 Go/C++ 服务测试结果伪称为本补丁重新执行。
- DEV Claude Code 三轮只读设计/代码/提交审查，确认无 P0/P1；设计轮发现的“声音选中但无音轨”同因路径已纳入 RED/GREEN。重复 include 已去除；未执行的物理显示/音频等边界保留。

## 证据、清理与剩余范围

本轮 DEV 证据根 `/var/tmp/qt-render-clock-VgLVQj/`，包含构建/CTest/RED/GREEN/Claude 原始输出、逐画面 JSON、截图、诊断原源码与日志，机器可读汇总 `render-verification.json`。TEST `/var/tmp/qt-dahua-a2fcc80-V2mC1Ksf/run-05-clock/` 保存独立 DB、服务日志、PID/starttime 与 API 结果；JSON 已复制回 DEV `test-results/`。私有截图、URL/凭据和原始日志均不提交，不公开归档。

测试结束后显式停止 live 返回 204，通道无 live，媒体 `/receivers` 返回空数组。仅停止本轮 media_server/signaling、Xvfb/probe 和身份核实的 SSH 转发；两项服务均退出 0。摄像机保留指向专用 TEST 平台。未删除历史失败、凭据或其他 Agent 资产。

结论仅为该呈现缺陷修复及所列实机路径通过，不代表七阶段平台整体 GO。真实云台动作、预置位写入、可用录像的回放/下载、seek/倍速、订阅、广播/对讲、物理桌面声音和正式发布许可等仍须按计划分别完成。
