# Web 管理界面真实使用验证

BASE_HEAD：`5f2021618b3ee5a7451692054e69001f30434aad`。修复版本：`093fd842b44409f452f52bb9779d05e7d69e7d82`（修复停止取流失败后的界面重试）。本记录不覆盖或替代已有质量验证报告。

## 环境与执行方式

2026-10-05 至 2026-10-06，在本机 loopback 上运行真实 signaling、media_server、GB28181 simulator、SIP、RTP/PS、WHEP 和 Chrome 153.0.8010.52。Chrome 在 Xvfb `:97` 中以有界面模式运行；使用 Playwright 操作真实控件，并用原生 X11 键盘事件验证 Chrome 缩放。没有 mock PeerConnection、新增调试 API 或自动恢复逻辑。

媒体服务使用 `build/media_server`、6 个 worker，RTMP/RTSP/HTTP 端口分别为 47220/47221/47222；signaling HTTP/SIP 为 47223/47224。模拟器固定单通道，设备规模依次为 1、5、20、50；50 台分成 1/4/15/30 台四个进程，用不同 loopback media-bind 避免其固定 RTP 源端口冲突。fixture 为 H.264，RTSP 使用稳定 paced H.264/AAC 发布者。

| 产物 | SHA-256 |
| --- | --- |
| media_server | `391b504d001c74852b51117411524f7bdba1c355c383a7e12ddf2de9d430a60b` |
| 修复后 signaling | `0c8898000ac3e2b98da82932e95c61530525a982c418d719ab64d78213a05859` |
| simulator | `c10edb362f0d6c9359621c20180c25e5bfc95d814116932a6862a7dddc9da2ee` |
| fixture.h264 | `38abf0d6ad0afbc794b6a580e156a796f71c36e0a299d828fd2922cad256e6c7` |

原始命令、journal、console、HTTP response、进程采样、日志和问题截图位于 `/tmp/media_server_dogfood_5f20216/`。这些临时证据不作为永久测试框架提交。

结构化结果见 [web_ui_dogfood.json](verification_results/web_ui_dogfood.json)。

## 真实交互与生命周期

初始交互从 22:14:34 到 22:46:46，共 32.21 分钟，从空数据库开始添加设备、等待上线、播放、关闭 viewer、重新播放、停止 live，并与 RTSP 添加、编辑、启动、播放、停止、删除来回交替。初次 soak 约 35.14 分钟后发现 P1 并中断，不计入修复后的通过时长。修复后的独立计时起点为 23:40:43。

实际使用覆盖：

- 鼠标、Tab、Shift+Tab、Enter、Space、Esc、方向键和 Home；弹窗、确认框、设备列表、表格 action、tab、播放器和返回入口。
- 快速 play/back/tab、close/replay、stop/play、跨设备切换、Esc 取消、重复 Enter、删除确认后切 tab。没有强制点击 disabled 控件。
- 1/5/20/50 台设备、46 online 与 4 offline 混合列表、长设备名、滚动及轮询后的选择与焦点保持。
- 三台不同设备独立 live/viewer；2/3/5 个独立 page/context 共享同一 live、各自 WHEP resource。关闭、刷新、切 tab、关闭 page/context 不停止共享 live；stop live 使所有相关 viewer 最终 ended。
- simulator 正常 Expires:0、SIGKILL、现有 expiry/timeout 离线、同 identity 重新 REGISTER；persistent device 保留，旧通道与旧 receiver 消失，手动重播创建新 generation。
- 播放中的 signaling 重启而不刷新网页：polling 失败后恢复，runtime registration/live 不恢复；重新 REGISTER 后手动播放成功。
- 播放中的 media_server SIGKILL/恢复、WHEP DELETE failure、play/stop failure；恢复后继续普通操作 12.88 分钟，并重新开始两小时 soak。
- 列表、详情、正在播放、failed、ended、RTSP tab、打开 dialog 前 reload；hash、back/forward；不恢复旧 selection/viewer，重新播放复用仍存在的 live。
- refresh 四次时业务 mutation 计数 148→148，INVITE 与正在播放的 resource 不变。
- 播放期间动态调整 1920/1440/1100/800/600/480/320 CSS 视口；在 1440×900 和 600×900 下验证原生 Chrome 80/100/125/150/200% 缩放。

主 A viewer 在修复后 soak 中连续保留同一 peer、video DOM 和 resource；其他页面承担 GB/RTSP、关闭、停止、刷新、dialog 等交叉操作。自然 transport failure 由正式 DELETE 该 viewer 的 WHEP resource 引发，15.58 秒后浏览器 peer 真实进入 failed，显示“播放失败／连接中断，请重新播放”；A 的其他 viewer/live 继续。手动重播复用 live，A INVITE 始终为 3，不使用事件替身。

后台验证使用独立 Chrome，断开 Playwright CDP 控制，仅以 console CDP 观察。后台 `document.hidden=true`、focus=false 持续 12.81 分钟；回来 hidden=false、focus=true、frames 3→18222，peer/live/resource 保持，INVITE 不增加。Playwright 控制页面的 focus emulation 不能作为真实后台证据，未用于这一结论。

## P1：停止失败后无法从 UI 补偿清理

真实复现路径：media_server SIGKILL → close viewer（DELETE 连接拒绝，本地 viewer 正常清理）→ 通道停止 live → receiver 清理不可达返回 502 → 服务端 cleanup_pending 映射为公开 `live.state=stopping`。请求已经结束、UI pending 已清除，但原代码仍因 `live.state === "stopping"` 禁用“停止取流”。恢复媒体服务后，同页及新页面均无法重试；269 秒后仍 disabled。正式 DELETE 可返回 204，说明后端补偿能力存在。

后端依据是 `live_session.go` 的 `stopSessionLocked`：cleanup_pending 可以再 DELETE，正在 stopping 的请求等待同一次操作，不重复 BYE；既有 signaling 文档也允许再次 DELETE 补偿。本轮未修改后端 fencing 或状态机。

修复仅删除 `renderChannels()` 中额外的服务端 stopping 禁用条件，保留 pending 防重复与非 streaming live 的播放限制。没有增加字段、helper、retry loop 或自动重播。生产变更为 `app.js` 一行替换，净 LOC 0；E2E 净增加 19 行；Go/C++ production 修改为 0。

在原 `gb_web_ui_verify.py` 固化真实媒体不可达/恢复 case：修复前 RED 精确失败于“failed cleanup must allow a user retry after media_server recovery”，前 20 checks 已通过；修复后完整 35 checks GREEN。本轮还在 headed Chrome 不刷新页面重做原始 SIGKILL 路径：失败后按钮可操作，恢复后 UI retry 清理成功，手动重播新 generation。RED/GREEN 截图保存于临时 evidence 目录。

## 回归结果

| 验证 | 结果 |
| --- | --- |
| 完整 `bench/gb_web_ui_verify.py` | 35 checks PASS，page_errors=[]，114.44 秒 |
| `bench/gb_simulator_verify.py --stage recovery` | 6 checks PASS，73.28 秒 |
| `go test -count=1 ./...` | PASS |
| `go vet ./...` | PASS |
| `go test -race -count=1 ./...` | PASS |
| `cmake --build build -j12` | PASS |
| `ctest --test-dir build --output-on-failure` | 14/14 PASS，22.65 秒 |
| ASan/UBSan | 未执行；C++ production 0 修改，按本轮要求无需重跑 |
| TSan | 未执行 |

Simulator recovery 覆盖 10 台 SIGKILL expiry、media crash/手动 stop/replay、signaling restart 持久设备、组合 restart、REGISTER refresh 从短暂不可达恢复、10 台 media unavailable create rollback/recover。

Browser GREEN 在提交前执行，result 的 HEAD 仍记为 BASE_HEAD；实测 `signaling/web/app.js` digest 为 `b74630cc0bbcbdcd7e2e7b7184fed3998b0c23e94a55632ca567cc582991ea53`，与提交 `093fd84` 的文件完全一致，测试的是修复后的实现。

## 长期资源与最终清理

修复后独立 soak 从 23:40:43 到 01:41:42，共 **120.98 分钟**。6 个固定服务/模拟器 PID 未重启。通常每 5～15 分钟进行不同组合的合法 UI 操作；一次上下文续接及只读检查使交叉操作间隔达到 19.15 分钟，保留这一执行偏差，不将本次描述为严格周期负载测试。未把故障与修复阶段计入通过时长。

终点主 A viewer 的 peer/video/resource 与 t0 相同：173,866 framesDecoded，framesDropped=0，packetsLost=0，nackCount=0，pliCount=0，bytesReceived=683,046,566。A INVITE 在独立 soak 中保持 3；最终额外端口复用试验才再次发起新 generation。

采样标签是目标时点，表内记录实际偏移。RSS/PSS 单位为 KiB；Chrome 是本任务所有 Chrome 子进程含 master/crashpad 的汇总，RSS 会重复计算共享页，应同时看 PSS。

| 实际分钟 | media RSS | media fd/socket/thread | signaling RSS | signaling fd/socket/thread | Chrome RSS/PSS |
| --- | ---: | --- | ---: | --- | --- |
| 0.002 | 28,948 | 33/10/6 | 24,032 | 10/4/20 | 2,159,380/732,840 |
| 31.566 | 38,560 | 35/12/8 | 27,020 | 12/6/24 | 2,282,728/858,666 |
| 61.858 | 38,772 | 35/12/8 | 26,516 | 10/4/25 | 2,436,384/984,506 |
| 120.965 | 39,032 | 35/12/9 | 27,400 | 10/4/26 | 2,790,604/1,333,785 |

160 次约 45 秒采样中，media RSS 37,100～39,296、fd 35～38；signaling RSS 25,220～27,760、fd 10～13。t0 是刚启动后的点，RTSP 与其他 viewer 入场后媒体服务达到高水位。三个区间的固定 PID CPU 平均 cores 分别为 media 0.01350/0.01278/0.01374、signaling 0.00331/0.00343/0.00352，没有随操作次数明显上升。UDP 内核 drops 和 tx/rx queue bytes 均为 0。

四个模拟器的 t0→t120 RSS 分别为 16,732→16,664、16,584→17,892、16,688→17,488、16,220→17,456；fd 始终 38/41/41/41，socket 始终 33/36/36/36，thread 分别 22→22、21→23、22→23、22→24。已有日志的 goroutines 始终 72/78/78/78。signaling 没有现成 goroutine 指标，本次只观察其 OS thread，不能声称验证了 signaling goroutine 数。

`phase_drops` 是 simulator 有界 media task queue 的调度 drop，不是 RTP send error 或内核 UDP drop。t0/t30/t60/t120 分别为：A 15/15/15/15、four 2/2/5/19、fifteen 1/1/1/1、thirty 0/0/1/1，独立 soak 新增 **18** 次。四个进程 rtp_dropped/send_errors/register_fail 都为 0；heartbeat_fail 保持 0/1/3/6，来自 t0 前的故障注入，没有在 soak 新增。未为更好数字修改模拟器队列或策略。

Chrome PSS 确有增长，不能写成浏览器内存稳定。t120 原始采样之后才对 5 个页面执行标准 CDP `HeapProfiler.collectGarbage`：PSS 1,333,785→1,158,305（回收 175,480），RSS 2,790,604→2,627,880。页面 JSHeapUsed 各约 1.9～2.9 MB，GC 后约 1.8～2.8 MB；DOM document/node 数也下降，listeners 不增加。该证据只能确认部分资源可回收，不能把剩余增长归因为某个具体 cache，不能据此证明 Chrome 或 UI 绝对无泄漏；浏览器退出释放内存也不作为无泄漏证据。服务器趋势与 Chrome 分开判断。

受控页面累计记录 console.error=179、console.debug=41、pageerror=0、warn=0。保留 92 次 HTTP 409（67 次 RTSP 既有 not-ready retry，其中 65 次集中于最初 fixture 未就绪；25 次 GB 首次 ticket 尚未就绪后按既有规则换一次 fresh ticket）、46 次 404、38 次 connection refused 和 3 次 502。18 个 debug 中的 TypeError: Failed to fetch 来自 signaling 重启期间已捕获的 polling 失败，另有明确 media_ended、webrtc_connection_failed、WHEP DELETE failure、live stop/start failure；没有把它们过滤为“console 无错误”，也没有未处理的 pageerror。

修复后固定 media 日志有 19 个 DTLS failed，均为已关闭/切换 viewer，同 ID 随后 shutdown，最终正式 GET 均 404；主连续 viewer 没有对应错误。固定 signaling 在 soak 前有 3 条 SIP 200 possible retransmission INFO，soak 中无未解释的错误。初始 SIGKILL/服务不可达期间的 BYE timeout、receiver start/stop 失败、RTSP shutdown refused 和发布者 Broken pipe 与故障时间对应。没有 queue full、no available media port 或未解释的重复异常。

最终清理证据：

- 通过 UI 停止 A/B live 与 RTSP source、关闭全部 viewer；正式查询 50 台通道均无 live，RTSP desired_state=stopped。
- 停 A 前申请一个未消费的新 ticket，复用当前 live；UI stop 后 0.039 秒内以真实 SDP POST 该 ticket 返回 404，证明 stop 失效，不依赖 30 秒 TTL 恰好过期。历史 ticket 已超过 TTL，过期机制由完整 E2E 覆盖，没有新增 ticket registry API。
- 当前两条旧 receiver 以准确 UUID+stream_name DELETE 均 404；最终全部 **56** 个从实际 response/journal/media log 取得的历史 WHEP resource，非破坏性 GET 全部 404。已用 active resource GET204 与 closed GET404 验证查询方法有效。
- 媒体 UDP 端口先全部释放；再从 UI 手动新 generation 播放 A 成功，分配 `[49156,49158,49159]`，其中 `[49156,49158]` 与此前占用端口相同。close viewer、stop live 后 UDP 再次为空，新 receiver DELETE404。
- 四个 simulator 正常 Expires:0，退出码均 0；50 台 persistent device 保留、online=0、channels=0。发布者正常终止返回 255（ffmpeg 信号退出约定）。
- 浏览器与发布者关闭后 idle：media RSS 38,904、fd 26、socket 3、UDP 0；signaling RSS 27,224、fd 8、socket 2。高水位未归零不单独判泄漏。
- 本任务 Chrome、模拟器、发布者、media_server、signaling 和 Xvfb 全部停止；后三者退出码均 0，没有遗留本任务 fixture 进程。原始日志保留。

## 接受的边界

P0、P2 未发现可稳定复现的新生产问题；P1 一项已修复。P3：600px 实际视口在 200% Chrome 缩放下为 300 CSS px，既有 html 最小宽度 320px 导致 20px 横向溢出，3/3 复现；按钮、dialog 和焦点仍可操作，按本轮冻结视觉边界记录而不修改。

当前 simulator 只有固定单通道与固定 channel name，8/20+ 通道密度、长 channel name 不在本次真实覆盖范围；未为测试增加模拟器功能。长 device name、合法长 RTSP stream_name/URL/username 已实际使用。

工具与环境问题包括 Playwright 元素 detached/actionability 等待、读取错误 selector 的 strict-mode 拒绝、完整导航后未重装临时观察对象、异步通道载入前立即 count、Chrome 内部 settings 页导致控制连接结束、多个模拟器共享固定 RTP bind、取流与 fixture 换代重叠。均保留真实失败记录并核对实际页面/API；未以产品改动适配工具，也未把未确认的 Chrome 退出根因归为产品崩溃。

最高可信范围是本机真实协议链路、这一 Chrome 版本及单通道 simulator 的连续交互与生命周期；不是 WAN、厂商互操作、多通道或绝对无泄漏保证。独立 CPU/RSS/fd/socket、Chrome 内存及 simulator drop 分类应结合最终采样读取，不能仅凭一次 allocator 高水位判定泄漏。
