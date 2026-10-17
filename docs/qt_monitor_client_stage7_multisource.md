# Qt 十六路独立源观看补验

## 范围与实现

2026-10-17，DEV `172.20.45.187`、i7-13700KF 上补验 16 个独立发布源，而非继续重复历史同源 16 格测试。基线 `b4b355f564a0b1dc691da439eee9a0180a6a093d`，仅 `desktop/tests/playback_probe.cpp` 有下述测试入口修改；生产播放器、Go、媒体服务器和鉴权不变。

现有探针的 `--push-device` 可供一次，保持所有格观看同一设备；也可恰好重复 `--views` 次，按顺序指定每格设备。拒绝空 ID、错误数量，以及与 `--source-id` / GB 目标混用。实际 `monitor_window` 模式先在设备树找到所有目标，再逐格触发原双击播放入口，不保存另一套运行状态。新增结果字段 `distinct_target_count` 只统计请求 ID，不能单独证明媒体独立。

已有逐格首帧、音视频 PTS/PCM、呈现进展、95% 帧率/呈现率和资源回收断言未放宽。没有给生产类新增状态、接口、线程或 helper。多 push 目标不等于混合 GB、主动拉流和 push 目标，也不代表不同编码内容。

基本调用方式（先准备四个真实 ready 的设备 ID，每个仍由平台独立签发播放授权）：

```bash
desktop_playback_probe --platform "$PLATFORM_URL" \
  --views 4 --seconds 30 --monitor-window \
  --push-device "$DEVICE_1" --push-device "$DEVICE_2" \
  --push-device "$DEVICE_3" --push-device "$DEVICE_4" \
  --output "$NEW_PRIVATE_RESULT"
```

`--audible --toggle-audio --stop-others-after-observation` 可补测声音开关及停止其余格；本轮使用认证 Xvfb/XCB 和 SDL dummy，不能把声音开关或 PCM 增长称为物理听音。本轮私有监督器的完整命令、进程 PID/starttime 和输入 SHA 均保留，不在 Git 提交带发布凭据的命令行。

## 实际负载与产物

16 个不同 push 设备、不同发布 token（即流标识）、独立 FFmpeg 进程；私有进程记录与设备发布地址映射逐一核对 16/16，媒体均 ready。8 路 H.264/AAC 经 RTMP 输入，4 路 H.264/PCMA 经 RTSP/TCP 输入，4 路 H.264 合法无音轨经 RTMP 输入。视频共享同一已编码 fixture，均 1920×1080、25fps、约 4Mbps；不是 16 台物理设备或 16 种画面。Qt 每格独立 RTSP/TCP 输出、CPU 解码。AAC 为 48kHz 双声道，PCMA 为 8kHz 单声道，播放 PCM 重采样到 48kHz。

DEV loopback 使用本轮独立端口 26680–26684，不是 TEST 或真正跨机流量；Xvfb 为 1920×1080、禁止 TCP 监听、使用私有认证，不修改用户桌面或 `.Xauthority`。不是容量、长期稳态或音画同步测量。

| 资产 | SHA-256 |
| --- | --- |
| 修改后的探针源码 | `f5c3365678716c06477892f9747618661d02d57bea57cd9a035fb0bc83463648` |
| 实际探针 ELF | `acfb2e082f82e0df81faf6ae05eedc51d16915f810ee00a3c5ead6598350fdc4` |
| 正常 C++ 媒体 ELF | `45f0c48affb7b866578178f5298fe1291d6c6efbeab7f79c34366f54dda4fde4` |
| 本轮 Go ELF | `549881626263387d8a69022ab936799d3be3818797fffaab682cd53c18321bfd` |
| 原 H.264/AAC fixture | `717951f2a7e2c49eab30f44a4a669d3c80318bc52a134dfb7f3f12eb64acfcb9` |

探针来自 `/var/tmp/qt-broadcast-sdk-755cf52-z2y4wEha/desktop-normal` 本轮成功增量构建；Go 在上述基线源码编译。C++ 媒体复用 `/var/tmp/qt-audio-sender-21949b9-xGQL3x5w/build/media_server` 冻结 ELF，不冒充本轮重新构建。没有重跑完整 CTest、Go 全量、ASan/UBSan 或第二机测试；本轮变更不涉及生产实现。`git diff --check` 与探针 clang-format dry-run 均通过。

## 三轮原始记录与结论

### 第一轮：观看通过，发布者 TERM 清理失败

证据：`/var/tmp/qt-multisource-b4b355f-lhkcnKW6/`。

三项参数负测（数量错误、空 ID、混合目标种类）均退出 2，未创建结果文件。实际旧单目标四格、新四目标 direct、新四目标 monitor 和 16 目标 monitor 四组 Qt 均退出 0。16 格共同窗口 `3021→33021ms`，恰 30 秒，1950 项断言通过、0 失败；每格新增解码/呈现 748–750 帧，有声音轨 PCM 新增 1,435,842–1,440,768。停止其他格后 15 个解码对象销毁，剩余格再观察 3 秒新增 75 视频帧；最后 99ms 关闭，无超时。截图确认实际 16 格测试图，动态由 PTS/帧进展记录证明，平台地址区域已遮盖。

外层结果仍为 **FAIL**：无限循环 FFmpeg 发布者收到 TERM 后未在监督器期限内退出，最终 15 个发布进程被 KILL；其余一个最终退出 255。所有本轮进程最终回收，但不能宣称发布者优雅停止通过。只读观察到部分 FFmpeg 处于 futex 等待，尚未定位原因，不能认定服务器或第三方缺陷。原始 `result.json.error` 含完整发布 URL，不公开复制。

### 第二轮：私有监督器错误

证据：`/var/tmp/qt-multisource-finite-b4b355f-vhPDA7iB/`，不包含其 `run-03` 子轮。

为独立验证有限源自然 EOF，预先限定 FFmpeg 输出 55 秒，不改变媒体 profile 或播放门槛。把多场景列表缩为单场景时遗漏 Python 单元素 tuple 的末尾逗号，Qt 启动前解包失败；原 **FAIL** 及清理记录保留。AST 对照明确旧循环迭代对象为五元素 tuple，修正后为包含一项五元素 tuple 的外层 tuple；不改产品或媒体断言。

### 第三轮：有限源自然 EOF 和资源回收通过

独立证据：`/var/tmp/qt-multisource-finite-b4b355f-vhPDA7iB/run-03/`。

监督器与 Qt 均退出 0，`result.json=PASS`。共同观察 `3124→33124ms`，恰 30 秒；1889 项断言通过、0 失败。原定时器按实际采样触发检查，采样间隔略有差异导致断言数量与首轮不同，并未减少门槛或缩短共同窗口。首帧轮询值为 2523–3124ms；不视为精密逐帧时延。

| 格号（从 0 开始） | 音轨 | 新增解码 / 呈现帧 | 新增 PCM 采样帧 |
| --- | --- | --- | --- |
| 0 | AAC | 750 / 750 | 1,437,696 |
| 1 | PCMA | 749 / 749 | 1,437,792 |
| 2 | 无 | 750 / 750 | 0 |
| 3 | AAC | 749 / 750 | 1,440,768 |
| 4 | AAC | 749 / 750 | 1,440,768 |
| 5 | PCMA | 749 / 749 | 1,437,792 |
| 6 | 无 | 750 / 749 | 0 |
| 7 | AAC | 749 / 749 | 1,438,720 |
| 8 | AAC | 749 / 750 | 1,440,768 |
| 9 | PCMA | 749 / 749 | 1,437,792 |
| 10 | 无 | 749 / 749 | 0 |
| 11 | AAC | 749 / 750 | 1,438,720 |
| 12 | AAC | 750 / 749 | 1,438,720 |
| 13 | PCMA | 749 / 749 | 1,437,792 |
| 14 | 无 | 750 / 750 | 0 |
| 15 | AAC | 749 / 749 | 1,440,768 |

所有路视频和音频丢弃计数为 0，无 decoder_error；解码/呈现增量相差一帧与窗口起点排队帧边界一致，但未单独逐帧追踪其因果，不推导为造帧。停 15 留 1 后对象销毁数 15，剩余格继续 3.1 秒，视频 750→827、PCM 1,484,800→1,633,280；最终 100ms 关闭，剩余解码线程 finished/destroyed、not running，无超时。

16 个发布者均按预声明 `-t 55` 自然结束并退出 0；删除本轮设备后配置为空、媒体 `final_receivers=0`。20 个自有进程（Qt、Xvfb、16 个发布者、Go、C++）全部退出 0、PID 消失，没有 forced kill；私有 X 认证文件删除，服务 TCP 端口可重新 bind。初始检查还覆盖 SIP UDP 端口，结束时监督器只复核 TCP 端口，不扩大为全部 UDP 端口泄漏证明。

Qt 进程 73 次短时资源采样的峰值 RSS 302840KiB、PSS 252168KiB、FD 51、线程 21，Swap 0。这只是功能负载的观测值，没有隔离/配对或容量基线含义，不证明长期内存无泄漏。

| 第三轮证据 | SHA-256 |
| --- | --- |
| `result.json` | `9c4861a50e77e67732452f527bad35f5d7a587b14c5333e2ff00d0c4b325ec13` |
| `monitor-sixteen-targets.json` | `3a463608b123427a900baec42eac791b5507f2a9f6a8ee822746bf188dbd1c8e` |
| `verify_multisource_v3.py` | `8de618b0c5fc1931ef8437b926dee45749e0961651d74060e21c835cafdd7d8d` |

## 未闭合边界与清理

有限源 EOF PASS 不替代或修复首轮无限源 TERM FAIL。未覆盖主动拉流/GB/push 混合目标、跨机 16 路、安装版外部操作、物理屏幕/声学、故障隔离或长期 churn。本轮补齐的是指定硬件与 profile 下 16 个独立源同时实际解码和呈现，不宣布完整阶段七或平台 GO。同会话录像 seek/倍速、真实双向声学及公共分发许可等原缺口仍保留。

所有原始输入、监督器版本、失败轮次、结果、私有源映射、日志、截图、ELF 与 SHA 保留；原始记录可能含凭据，不提交 Git。只允许清理本轮生成且后续不需使用的可再生缓存/中间对象，不动其他 Agent 目录、归属不明资产或历史原始证据。

本阶段结束只删除本轮正常构建重新产生的 12 个 `.o`，逻辑大小 33,445,432 字节、磁盘占用 33,472,512 字节；删除前核对精确路径、大小、所有者、非符号链接且无对应构建进程。`cleanup-owned-objects.json` 保存逐项 SHA 和清单，探针 ELF、Qt JSON 和总结果删除前后 SHA 不变；需要时可用原 CMake 配置重建，不可恢复旧 `.o` 本身。第一次清理脚本因宿主 Python 不支持 `hashlib.file_digest` 在任何删除前退出，修正为分块 hash 后执行成功，旧脚本保留，不将此工具错误当作产品失败。
