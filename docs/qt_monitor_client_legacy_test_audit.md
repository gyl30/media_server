# Qt 监控客户端遗留实现与测试审计

## 1. 审计范围与版本

本轮从 `2bb192a` 的 Qt 客户端、GB 报警、有限回放、公共对讲及 Web 对讲路径进行定向审查，随后复核 `6267b6d`、`15fb631` 和 `321fcf1`。本文记录的是这些实际调用链及测试资产，不是全仓死代码、协议覆盖或安全性的穷尽证明。

工作区为 `/home/gyl/example/media_server_qt`，分支为 `feature/qt-monitor-client`。本轮原始证据保存在 DEV：

```text
/var/tmp/qt-final-audit-2bb192a-duu1a2Of/
```

取证阶段先只读核对调用链，再对已证明的问题作下述最小修改；不重新执行容量或严格声学试验。并行完成的公开对讲摄像机功能验证单列于[实机报告](qt_monitor_client_dahua_public_talk.md)，不把数字发送证据扩大为声学结论。源码行号对应本轮审计快照，后续提交可能使行号移动；函数名和文件路径用于再次定位。

## 2. 已确认且已完成的最小清理

| 提交 | 改动 | 审查结论 |
| --- | --- | --- |
| `15fb631` 删除对讲返回结果中的旧直送字段 | 删除 `broadcastResult.sourceID/peer/ctx` 三个返回副本及旧媒体调用方注释；同步增强原测试 | 唯一生产调用方 `gb_talk.go` 只消费 `mediaID/uploadToken`；内部 SIP 会话仍保留必要身份、端点及取消上下文，没有删除运行期事实 |
| `6267b6d` 隔离浏览器回归的调试连接归属 | CDP 只连接本次 Chrome 的 stderr 公布的端点，默认动态端口；新增归属回归及使用说明 | 修复可达的测试归属错误，未改业务覆盖或生产媒体路径 |
| `321fcf1` 区分对讲探针首媒体与稳态观察 | 独立5秒首媒体期限，再开始完整观察，记录首媒体延迟；原正常/早断/非法参数及精确DELETE保留 | 生产`transmitting()`不是首PCM承诺；修正测试测量分层，不降低25包/秒、末进展或错误门槛。run05旧FAIL保留，run06单列新观察 |

**本轮删除测试数量：0。** 没有因为测试耗时、依赖私有素材或未注册默认 CTest，就将其认定为废弃代码。

`gb_broadcast_test.go` 保留 Notify 的 SourceID 断言，并改为检查真实出站 `create/configure` 调用：媒体 ID、上传 token、远端地址、RTP/RTCP 端口和 SSRC。取消测试从实际 `broadcastSession.ctx` 观察原因，先等待会话结束，不再依赖已经无人消费的返回副本。这不是删掉端点或取消覆盖。

## 3. Go 与媒体侧必须保留的实现

### 3.1 公共对讲只有一条生产媒体路径

实际链路为 `signaling/infrastructure_http.go:77–79` → `gb_talk.go:22` → `gb_broadcast.go` 的 `runBroadcast()` → C++ 音频会话控制；Web 则由 `web/detail.js` 调用 `web/talk.js`，经已授权 WebSocket 上传。

Go 的既有 `broadcasts` 保存 SIP 会话所有权，C++ 音频会话保存媒体资源。公开 POST 不接受任意媒体端点；`gb_talk_test.go:25` 保留了拒绝这类请求的断言。未发现仍然可达的 Qt/Web 直送摄像机 UDP 的生产并行入口。旧 result 字段是返回契约残留，不代表内部 `sourceID`、`dialog.peer`、`session.ctx` 也无用。

### 3.2 不应删除的状态、组件和回归

| 对象或测试 | 实际职责与证据 | 决定 |
| --- | --- | --- |
| Broadcast 内部身份、dialog、ctx | `gb_broadcast.go` 保存 Notify/反向 INVITE 关联、SIP 对话及终止原因；`gb_broadcast_test.go:268–616` 验证来源、SN、ACK、BYE 和旧对象隔离 | 保留 |
| HTTP 成功交接与请求取消 | `gb_talk_test.go:31–312` 覆盖成功后独立寿命、取消、响应写失败、精确 ID 删除和 reconcile 替换 | 保留，不能合并两种寿命 |
| 报警 opaque cursor | `alarm.go` 的代际与序号表达不同事实；`alarm_test.go:16–201` 覆盖重启追平/超越、分页缺口、有界缓存和真实 UDP 来源 | 保留；`latest` 诊断值不能代替分页 cursor |
| 有限回放票据与控制 | `gb_playback_end_test.go:278–365` 验证真实 20 秒 drain；`gb_playback_control_test.go:218–323` 覆盖 INFO/BYE 串行与取消 | 保留，不能用直接 shutdown 冒充期限耗尽 |
| 音频 sender 与 session | `gb28181_audio_session.cpp:35,52,162` 分别创建 sender、配置 peer、提交合法 PCMA；`http_session.cpp:141–169` 是控制与上传入口 | 当前产品依赖，不是旧直送遗留 |
| 预绑定端口后配置 peer | 音频 sender 的端口先供 SIP 协商，再配置真实对端 | 两阶段来自实际协议依赖，不能为统一 startup 合并 |
| 同步 packetizer 的临时 buffer | `gb28181_audio_sender.cpp:255–271` 使用同步 packetizer API；`udp_transport.cpp:61–79` 将 span 复制到持有队列的 vector | 当前异步发送不借用临时 array；不能机械删除配套回调 |
| 两个音频 idle timer | 控制租约和合法音频进展各自 20 秒，分别防止失去 Go owner 或无音频的残留 | 不合并为一个期限 |
| 在途 WebSocket 对象和强回调 | `upload_` 关闭 socket 后仍须活到 Beast completion；owner 清理时解除 sender 回调 | 资源持有不是无依据的重复关闭标志 |

`tests/gb_audio_sender.cpp` 覆盖 RTP payload/SSRC/序号/时钟、真实 RTCP、丢新包后恢复、端口失败清理、异端点 RTCP、BYE、真实异步发送错误及最终 weak 释放；`tests/gb_audio_upload.cpp` 覆盖跨 worker 配置、一次认领、非法帧、删除/停机/迟到回调和两个独立期限。两层测试职责不同，均保留。CMake 分别以 `gb_audio_sender` 和 `gb_audio_upload` 注册，超时为 30 秒、90 秒。

公开上传的控制续租测试同时要求真实 RTP 持续到至少 28 秒、收到至少 100 包、10 秒续租返回 204，并在至少 29 秒后到期；另一项在持续 status 而无合法音频时约 20 秒回收。它们分别证明续租有效及音频期限独立，不只观察最终 404。

本范围未确认新的生产可达 P0/P1，不据此推导所有协议组合已经验收。

### 3.3 以实际执行模式驳回的清理假候选

审查私有Pulse监督器时曾怀疑`trap cleanup EXIT`吞掉清理非零。不能省略真实脚本的`set -euo pipefail`就认定缺陷：DEV同版Bash的8项隔离实验，普通模式确实吞码，但真实严格模式的`return 1`/`false`退出1、失败`wait`退出9。提取run06原完整cleanup/trap，使用本次自有、已回收的有限子进程也退出9，原脚本SHA前后相同。因此**不改脚本**，不增加清理包装或状态。

`talk-exit-trap-receipt.json` SHA256 `164e8074d7145b07c3ab7b4acae937b8caadc8b9b88697271e1b03e949052e2d`；配套`audit_exit_trap.py`保留。独立只读核对run06 Pulse PID已消失，Qt play/talk PID及PGID无成员；没有发送信号或重新启动服务。严格模式可能替换原非零错误码，但不会把清理失败变成PASS。此例说明必须先复现真实执行前提，而不是为理论候选强行造修复。

## 4. Desktop 候选审查与保留理由

| 候选 | 真实调用与职责 | 结论 |
| --- | --- | --- |
| `presentation_rate_` 与普通有限回放分支 | `playback_view.cpp:254` 在 Scale 模式启用；`:553,685,917` 分别处理普通/Scale 时钟和队列，`monitor_window.cpp:533–535` 有真实按钮入口 | 普通有限回放和协议 Scale 均可达，不删任一模式 |
| `finite_http_input` | `decode_thread.cpp:151` 用于回放，`record_thread.cpp:156` 用于下载；`finite_http_input.cpp:138–195` 区分完整 chunked EOF 和异常断线 | 两个真实消费者，保留 |
| `request_history` | `playback_view.cpp:127`、`record_query_dialog.cpp:267` 两个调用；`history_request.cpp:20–25,36–78` 固定原平台、响应 context，并补偿迟到实例 | 不是仅调用一次的无意义包装 |
| `record_thread::finish_requested_` | `record_thread.h:40–55` 区分实时录制停止并保存与有限下载中断取消；`playback_view.cpp:473` 和 `record_query_dialog.cpp:316` 分别触发 | 独立业务事实，保留 |
| image/stats 及帧计数访问器 | `playback_view.cpp:615–623` 的图像、状态、截图和呈现计数仍被产品及探针消费 | 测试观察用途不等于废弃 API |
| 独立 probe 可执行入口 | `desktop/CMakeLists.txt:33–36,85–87` 构建；`talk_session_test.cpp:179` 真正启动子进程；`desktop/README.md:56–70` 提供外部 CLI | 未直接注册成 CTest 不等于孤儿工具 |
| 运行包测试 | `desktop/tests/package_runtime_test.py:40–45` 要求显式 `INTERNAL_RUNTIME_DIR`；`docs/qt_monitor_client_runtime_proxy.md:15–22` 有手工命令 | 依赖新运行包，不能假称默认 CTest 已执行，也不为形式删除 |
| `desktop_media` 生产源文件 | `desktop/CMakeLists.txt:15–20` 纳入媒体库，经 `main.cpp:19` 到 monitor window 的播放、录制与对讲入口 | 本范围未证明存在无调用的生产 cpp |

必须保留的回归包括旧 finished 不抢新音频借用、未来 PTS 饥饿、有限回放取消/迟到响应/原平台归属、seek/Scale 失败与时间戳 epoch、实际 SDL/swr 后的窗口与音频进展，以及探针超时后安全清理。对应位置包括 `playback_view_test.cpp:183`、`live_playback_view_test.cpp:270–280`、`finite_playback_view_test.cpp:81–234,679–1226`、`scale_playback_view_test.cpp:117–162,196–209,875–921`、`talk_session_test.cpp:153–221` 和 `playback_probe.cpp:412–445`。

## 5. CDP 归属问题与 RED/GREEN

### 5.1 真实问题

旧 `tests/web_console/cdp.mjs` 默认固定端口 9333，虽然启动了自己的 Chrome，但通过端口上的 `/json/version` 发现目标。如果该端口已有浏览器，就可能操作并非本轮启动的 Chrome，使 profile、fake audio 或页面证据失去归属。旧清理未做全局 kill，但这不消除误连及假证据风险。

最小修复仅信任本次 spawn 的 Chrome stderr 中完整的 `DevTools listening on ws://…/devtools/browser/…` 行；默认端口改为 0，也保留显式独立端口。stderr 解析缓存有界，不再盲连既存 HTTP 端点，没有新增浏览器管理框架。

### 5.2 可重复证据

证据子目录：`cdp-ownership/`。RED/GREEN 均使用本轮自有假 Chrome 和自有 HTTP/CDP sentinel，不连接任何他人 Chrome。

| 检查 | 实际结果 |
| --- | --- |
| RED：旧 harness | sentinel HTTP 请求 1 次、WebSocket upgrade 1 次；归属断言退出 1，证明误访问 |
| GREEN：修复后 harness | sentinel HTTP 0 次、upgrade 0 次；回归退出 0 |
| 假 Chrome 自身启动失败 | 两轮 harness 退出 1 是预期启动失败，不将它伪装为浏览器成功 |
| 真实 Chrome 默认动态端口 | 自有 profile、`about:blank` 执行 `6*7 == 42`，退出 0 |
| 真实 Chrome 显式独立端口 | 同样观察，退出 0 |
| Node 语法检查 | harness 和回归脚本通过 |

真实浏览器为 Chrome `153.0.8010.52`，Node `v24.13.0`。smoke 有 20 秒外层期限和 5 秒强制结束余量，没有媒体服务；它只证明浏览器启动、归属与 CDP 可用，不能算真实视频解码验收。

可核对文件：`red-final.log`、`green-final.log`、`chrome-default-final.log`、`chrome-explicit-final.log`、`result.json`，以及保留的首次日志和 `baseline/tests/web_console/cdp.mjs`。提交时脚本 SHA256：

```text
937750fd48579e2501f37487b79a82ebdda2ebbdc150abc1dea4e07ab8446d3a  cdp.mjs
57650c9da34d1db7cd4b5ca568c00884c8ad5cfed30fddfb4cd257b6e011bebe  cdp_ownership_test.mjs
```

## 6. 不能混同的测试证据与 Skip

- `gb_talk_media_test.go:30–33,239–241` 和 `gb_talk_web_test.go:21–25` 的 C++/Qt/Web 整链依赖显式环境及实际 probe；普通 Go 测试通过不能说明这些条件分支全部执行。
- `tests/web_console/talk_worklet.mjs` 是独立 Node 命令，不属于默认 Go/CTest。实际被产品加载的是 `web/talk-worklet.js`，经 `talk.js:38,41` 使用，并非孤立测试模块。
- `gb_talk_media_test.go:341–370` 可验证数字 PCMA 包、序号、时间戳及 dummy silence；只有设置 `GB_AUDIO_TEST_CAPTURE_TONE_HZ` 才执行 `:373–390` 的音调/RMS 检查。强制提供 WAV 不等于强制非静音声音，不能仅据包数称为听音通过。
- Web 的取消与迟到 getUserMedia、单 credit worklet、有界消息和精确 DELETE 均有实际生命周期职责，没有因名字或表面重复而删除。
- `decode_thread_test.cpp:37,304,369`、`finite_playback_view_test.cpp:324`、`scale_playback_view_test.cpp:217,388` 等 QSKIP 依赖私有 PS/短尾 fixture 或 FFmpeg。汇总退出 0 不代表 Skip 的媒体断言执行过；须对照本轮实际 fixture 配置和日志。
- 票据 TTL 测试中有可控时钟及短期限真实 timer，也有保留 60 秒票期但立即 shutdown 的案例；不能将它们描述为真实等待并验证了所有 60 秒耗尽行为。

## 7. 本轮实际验证快照

| 项目 | 状态 | 实际证据 |
| --- | --- | --- |
| Go 定向 | PASS | `go-directed.exit=0`；signaling 8.291 秒 |
| Go 全量 | PASS | `go-test.exit=0`；signaling 142.398 秒、simulator 0.003 秒 |
| Go race | PASS | `go-race.exit=0`；signaling 148.265 秒、simulator 1.007 秒 |
| Go vet | PASS | `go-vet.exit=0`，无诊断 |
| 后端音频 Normal | PASS | `audio-build-ctest.exit=0`；sender生命周期、音频sender、公开上传3/3，65.71秒 |
| 后端音频 ASan+UBSan+LSan | PASS | `audio-asan-ctest.exit=0`；同上3/3，65.86秒，无sanitizer错误 |
| CDP 离线归属 RED/GREEN | PASS | 上节独立原始日志与退出码 |
| 自有真实 Chrome 两种端口 | PASS | `chrome-*-final.log`；只属于基础 CDP smoke |
| talk worklet 离线检查 | PASS | `talk-worklet-correct-entry.log`、`talk-worklet.exit=0` |
| Desktop Normal 全套 | PASS | `normal-ctest.exit=0`，10/10、482项QTest零失败/跳过，982.10秒；详见下述版本与日志边界 |
| 新版首媒体探针 Normal | PASS | `first-media-normal-ctest.exit=0`，1/1、17项QTest零失败/跳过，21.92秒 |
| 新版首媒体探针 ASan | PASS | `first-media-asan-in-full-ctest.log`中17项零失败/跳过，22.18秒；也包含在下行全套 |
| Desktop ASan+UBSan+LSan 全套 | PASS | `asan-ctest.exit=0`，10/10、482项QTest零失败/跳过/blacklisted，987.29秒；原始日志无ASan/LSan错误或UBSan runtime error |

worklet 首次误用了不存在的 `talk_worklet_test.mjs`，`talk-worklet.log` 保留 `MODULE_NOT_FOUND`。这是入口拼写错误，不能隐藏为 PASS，也不是生产音频缺陷；纠正后实际执行 `talk_worklet.mjs`。正确日志验证 65,536 个 PCM 值与 FFmpeg 一致、44.1/48kHz→8kHz、credit 有界和恢复新鲜度，并拒绝缺失或目录音频素材。

可复核命令如下；本文不把这些命令本身当作新的执行记录：

```bash
cd /home/gyl/example/media_server_qt
node tests/web_console/cdp_ownership_test.mjs
FFMPEG_TEST_EXECUTABLE=/home/gyl/bin/ffmpeg node tests/web_console/talk_worklet.mjs
```

Normal/ASan 的实际监督脚本为证据目录中的 `final_desktop_checks.sh`，日志及最终 `*-ctest.exit` 已由主执行代理完成后核对。该流程使用独立已有构建、显式有限 PS/短尾素材及 FFmpeg，ASan 开启 leak 检测；路径中的工具链目录名不是实际编译器版本证明。`final_audit_receipt.py`逐项校验零退出、十组Totals/482项/零skip和blacklisted、三个后端测试实际Passed及sanitizer诊断；结果和原始文件SHA保存在`final-audit-receipt.json`与`final-audit-SHA256SUMS`。它不是新的生产测试框架，也不以当前Git HEAD冒认所有二进制来源。

Normal全套先构建`2bb192a`桌面代码，其中原探针17项随后另以新版定向补跑；其余生产桌面代码没有变化。ASan全套开始前重新构建新版探针。不能把前一个Normal探针结果冒称新版已测，也不把工作区报告改动当作已运行程序内容。

实际编译器是`/usr/bin/g++-16`的GCC16.0.1 experimental，不以目录名`gcc-16.2`推定版本。桌面ASan目标实际参数含`-fsanitize=address,undefined -fno-omit-frame-pointer -O1 -g -DNDEBUG -std=gnu++20 -Wall -Wextra -Werror`；环境启用`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`、`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`及`LSAN_OPTIONS=exitcode=23`。外部Qt、FFmpeg和系统库未全部重编插桩，本结论不能扩大为所有第三方内存安全证明。

后端复用既有、通过项目兼容守卫的独立Boost1.92 ASan前缀，采用Debug构建；`audio-asan-LastTest.log`中保留两条ASan对`makecontext/swapcontext`支持不完整的标准警告。警告不等于内存错误，但也是验证边界，不能以退出0声称运行库没有任何限制；本轮没有关闭报告、关闭泄漏检测或绕过守卫。

本轮执行器误在同一Normal构建目录重叠启动了定向CTest，导致`LastTest.log.tmp`被后一个CTest换名，常规`normal-LastTest.log`只有定向组；不能拿它当全套细节。全套原进程仍持有完整已unlink日志，已从该自有进程文件描述符只读跟随保存到`normal-full-recovered.log`（有完整十组、十个Totals及End testing）；其正式汇总`normal-ctest.log`从未丢失，退出0。后续不在同一构建目录重叠运行CTest，原始错误记录及完整恢复日志均保留，没有重写失败或改变测试结果。

## 8. 未完成边界与最终限制

1. 本轮审计快照尚未补齐 probe 的 no-first-media / mid-stream-stall 故障边界，不以已有正常 probe 结果替代这些负对照。
2. 数字 PCMA/RTP、WS 包增长或回传采集不等于摄像机扬声器实际发声、物理麦克风或同步双向声学通过。已有物理声学结论仍为 INCONCLUSIVE，不能据本文升级。
3. Qt 发布许可与第三方分发闭合不在本轮审计范围，不能据代码/测试清理给产品发布 GO。
4. 本轮没有证明所有输入×输出、真实 NVR seek/Scale 或全部 PTZ 动作都通过，也未运行新的容量或长期稳定性测试。
5. `desktop/README.md` 中关于 seek/倍速及笼统验收状态的过时表述由主执行代理单独修订；不改写历史实验在当时未执行的事实。

结论：已删除的仅为三个经调用链证明无生产消费者的返回副本，已修复可复现的浏览器测试归属错误及探针测量分层，未删除任何测试。保留项都有实际产品、异步所有权或独立测试职责。独立只读复审没有确认新的P0/P1；本轮局部变更和上述实际回归通过，不构成整产品GO。未删除原始证据或失败日志，未修改main、第三方依赖或相机配置。
