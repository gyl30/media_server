# 多协议 Web 控制台接续验收

本报告记录当前分支的实际验证，不继承 Claude 历史测试或 main 的 Phase 7 结论。
完整验收不只取决于通过数量：原始音频 PTS 失败仍未唯一定位，不能宣称 GO。

## 1. 基线与证据

- BASE_HEAD / CLAUDE_HANDOFF_HEAD：`c5a4ae0d8011997b498387eb83097e0e80c32f50`。
- 当前已验证生产提交：`03748c9`；最终文档提交及 FINAL_HEAD 见最终交付。
- 分支：`feature/web-console`；唯一开发工作区：`/home/gyl/example/media_server_web`。
- main：`dbd0e589ad66ba44cfa599535064dbf9ef958dd9`。
- origin/main / 共同祖先：`cafee7e28f96c3d8d998bb0191734a643edb9571`。
- 未 push、merge、rebase、amend，未修改第三方或其他 Agent 的工作区。
- 受限证据根目录：工作区内 `.tmp-codex-multi-input-20261014/`。原始日志、票据、数据库和截图不提交。
- 接手快照及 Claude 历史边界：[web_console_handoff.md](web_console_handoff.md)。
- `evidence-sha256sums.txt` 列出 552 份选定的原始日志、JSON、脚本、截图和合成素材，逐项 `sha256sum -c` 通过；不声称覆盖所有 build 中间产物或浏览器 profile。
  清单 SHA256：`c314e6ec0c4ae53086ba7d2cf4c546b3e9a6e90cfa88425f68894505ef7e4232`。
  运行 ELF 独立列于 `runtime-sha256sums.txt`。首次生成因 glob 顺序把清单自身纳入而校验失败，
  初版与失败日志均以 `*-initial-self-included.*` 保留；修正文件选择后再校验，没有改写受测日志。

DEV 使用 GCC 16.0.1 experimental、Boost 1.92 静态、`/home/gyl/ffmpeg901` SDK。
独立源镜像只读引用已核对 gitlink 的第三方源码，未在本工作区初始化或修改子模块。
Normal 为 RelWithDebInfo（`-O2 -g -DNDEBUG -Werror`）；另有独立 Debug ASan/UBSan 构建。
真实媒体环境在 DEV 的隔离 loopback 地址和端口运行，不冒充跨机器、容量或真实摄像机测试。
`build-source-final-check.json` 核对本轮八个 CMake/C++/Python 编译或测试输入与独立源镜像逐文件一致。
Normal 媒体 ELF SHA256 为 `5fa59954dc6b2433f8fa8ca58dc3e3f20015eeaeb6f4b2a0253ec636a94476ef`，
最终 Go ELF 为 `08bfd9362a5870873ad924798b452712c2c5a5d2adf67f4627ab2b2f2a3825ab`；
运行参数和原始身份见各环境的 `processes.json`，UI 字节一致性见第 6 节。

## 2. Claude 恢复与本轮提交

保留 Claude 已完成的 GB 通道失败循环修复、300 秒被动地址、RTSP 自动接入、RTMP Play 客户端、
WHEP HTTP/ICE/DTLS/SRTP 客户端和七协议添加界面。没有重建协议实现、设备后台模型或 registry。
原先未提交的监控改动和新 `player.js` 已保留原始副本，再接续完善。

| 提交 | 内容 |
| --- | --- |
| `12fd843` | 接手状态与验证边界 |
| `5b694ce` | 未就绪源拒绝签发播放地址；补跨 SQLite 连接原子消费和重开回归 |
| `fa5fdcd` | WHEP 完整 STUN 事务/端点匹配、独立 consent、SDP 前置约束、停机 DELETE |
| `3fcbaf0` | 候选与已绑定 UDP socket 地址族一致；不增加备用 socket 或回退路径 |
| `52b726b` | 原生测试客户端使用真实 client SRTP 角色；不修改生产协商 |
| `f8afeb8` | HTTP-FLV 测试客户端关闭 socket，终止迟到的继续读取 |
| `03748c9` | 详情/监控共享播放器；迟到路由和缓存 GB 代际修复；更新真实浏览器回归 |

主要 RED/GREEN：完整 STUN 事务伪造、无效 SDP 方向/BUNDLE、混合地址族选路、worker 停机 DELETE、
详情离页后的迟到 update、停止 GB 后缓存旧 live 的监控竞态，均保留原始失败及修复后证据。
HTTP-FLV 工具的 5 秒退出超时先复现为 10 次中 1 次失败，最小修复后 20/20 与健康测试 3/3 通过。

## 3. 功能和授权语义

三种主动源共用现有 source 配置、运行意图与 generation。创建自动连接，失败保留配置，不无限重连。
RTMP 真正握手/connect/createStream/play，WHEP 真正 HTTP、controlling ICE、DTLS fingerprint、SRTP 和 RTP 解包。
WHEP 的 Opus 输入通过既有组件转换为内部 AAC，浏览器输出继续使用原共享 AAC→Opus egress。

被动地址在确认前只存在于表单准备 registry，不能发布；确认后数据库条件 UPDATE 是消费的唯一权威。
确认时开始 300 秒期限，首次成功授权即永久消费，已建立发布不受到期影响。
播放仍为独立 60 秒一次性票据，不统一改为 300 秒。推流中禁止重生成；旧授权原子失效。
Go 回归覆盖旧表迁移、幂等重开、可控时钟、确认/取消、过期、重生成、跨两连接并发与重启。

UI 保持 V3，不展示五种播放 URL 或“签发地址”。详情显式本页播放；每个画面独占 video、PeerConnection、
DOM 和回调，布局只移动画面对象。关 viewer 不停止其他观众共用的上游。
GB 始终按设备/通道点播，由后端固定当前 live，不把旧缓存 live_id 用作新授权目标。

## 4. 构建和自动回归

| 项目 | 实际结果 | 证据（相对证据根目录） |
| --- | --- | --- |
| 完整 Normal 构建、benchmarks ON | PASS | 独立 `build-normal/`、构建日志 |
| 完整 Normal CTest | PASS，71/71，454.51 秒 | `ctest-final-flv-fixed-full.log` |
| Go test / vet / race | PASS | `go-final-ui-{test,vet,race}.log` |
| 10 个生产 JS、16 个测试 MJS 语法 | PASS，26/26 | `js-final-check.log`；实际 `node --check`，未替代浏览器运行 |
| ASan 兼容守卫与完整 Debug 构建 | PASS | `configure-asan-final.log`、`build-asan.log` |
| 完整 ASan CTest | 原始执行 FAIL，65/67；两个环境边界 BLOCKED | `ctest-asan-full.log`、`asan-failure-review.QZ64FtMm/` |
| UBSan 完整构建、纯 SDP | PASS，SDP 1/1 | `build-ubsan/`、`ctest-ubsan-sdp-final.log` |
| UBSan 跨 worker、媒体、RTMP/RTSP 拉流生命周期 | PASS，4/4，8.14 秒 | `ctest-ubsan-lifecycle-final.log` |
| UBSan TCP 顺序、背压、错误、在途与重入关闭 | PASS，6/6，0.07 秒 | `ctest-ubsan-tcp-final.log` |
| UBSan UDP、WebRTC 授权、WHEP 生命周期/DTLS、三种真实拉流与失败 | PASS，16/16，141.96 秒 | `ctest-ubsan-network-final.log` |

ASan 使用只读的 Phase 7 Boost 1.92 ucontext/ASan 前缀，兼容守卫真实通过，开启 address+undefined 与 LeakSanitizer。
新增 RTMP/WHEP 正常解码、失败、事务、consent、候选及关闭路径在完整运行中均通过。
两个失败分别为：RLIMIT=64 的 EMFILE 故障场景令 sanitizer 自身 pipe 创建失败；
accept_errors 的 fault LD_PRELOAD 抢在 ASan runtime 前面。独立复现和正确预加载顺序的对照保留，
不排除测试、不禁用 leak detection、不称完整 ASan 全通过，也不把该环境失败归为生产 UAF。
外部 FFmpeg、OpenSSL 等普通库未全部插桩，项目通过不能外推为所有依赖的内存安全证明。

## 5. 真实双轨与浏览器

`real-final/` 中一个独立上游、三种真实拉流、三种真实发布、一个合成 GB 设备。
H.264/AAC/Opus 为 1280×720、25fps 合成素材；GB 为 640×360 H.264 + 8kHz PCMA。
Native WHIP 发布与 WHEP 拉流均经过真实网络握手，不使用静态 SDP、假 RTP、伪造 getStats 或 iframe。

| 输入 | 实际 FFmpeg 视频输出帧 | 实际音频样本 | 结果 |
| --- | ---: | ---: | --- |
| RTMP 发布 | 76 | 144384 | PASS |
| RTSP 发布 | 76 | 146432 | PASS |
| WHIP 发布 | 76 | 142336 | PASS |
| RTSP 拉流 | 76 | 143360 | PASS |
| RTMP 拉流 | 76 | 143360 | PASS |
| WHEP 拉流 | 76 | 143360 | PASS |
| GB28181 | 76 | 23680 | PASS |

每目标 3 秒，一次独立新票据；保持双轨、实际解码、尺寸、视频/音频 PTS 严格递增和无 demux/decode 错误断言。
证据：`real-final/ffmpeg-seven-final-20261014/seven-result.json`（7/7，无自动重复）。
不得用该通过删除先前真实 PTS 失败，见第 8 节。

最终 Chrome 共 357/357：详情 22、七输入真实播放 102、播放与重放 22、真实授权到期 7、详情退出 8、
设备管理 75、系统 19、剪贴板 4、移动端 9、通道失败循环 9、四档布局 32、单/四画面与快速替换 48。
对应证据均在 `real-final/browser-*/result.log`，截图和原生统计保留于各自目录。
布局为 1920/1440/1024/390；不继承旧 183 项或旧 32 项结果。
Firefox 原始 7/8：页面通过，能力列表无 H.264，真实播放失败，媒体验收 BLOCKED；未安装或绕过 codec 限制。
上述 video/video_entry 的 `pagehide` 由测试显式派发，证明真实 handler 清理，不冒充实际关闭标签页的独立网络观测。

另外进行了唯一一次独立真实关页补测 `real-tab-close/evidence/`：77 项 PASS、1 项 FAIL，整场保留 FAIL，
不合并进上述 357 项。真实双轨帧、samples 和 energy 增长后执行 Chrome 原生 `page.close(run_before_unload=True)`；
按单调时间确认关闭阶段的 trusted beforeunload/pagehide，排除首次导航事件，服务端有精确匹配资源的 shutdown。
但 context 没有观察到该资源 DELETE 请求/响应，完成 DELETE 的断言 FAIL。
这里的结论是 NOT_OBSERVED，不能推导请求未发送，也不能凭此认定生产故障；关闭后 publisher-ready 断言尚未执行。
详见 `failure.json`、`real-tab-close-observation.json` 和 `final-readonly-review.json`。

三 viewer 专测 `real-final/gb-three-viewers-20261014-v2/result.json`：64/64。
三个独立资源共享一个既有 live，INVITE 增量 0；视频帧和音频 samples/energy 都持续增长。
关一个，其余继续；全关，上游保留；显式停止才移除 receiver；新播放建立新代与一次新 INVITE。
DELETE 精确匹配资源，实际 HTTP 404（DTLS 关闭先清理），不是虚称 204。
第一版 helper 在 Peer 关闭后立即检查 DELETE，发生可达测试竞态；保留 FAIL，第二版只补有界响应等待，不放宽断言。

## 6. 重启与最终清理

`real-final/restart-v2-20261014/` 控制 75/75：相同 source_id、运行意图恢复为三张新 generation；
停止源仍停止，旧 receiver 被删除，旧推流凭据仍已使用，不恢复旧媒体会话。
重启前未消费地址保留原 URL 与截止时间，重启后由真实 FFmpeg 首次消费；未确认 preview 和旧已消费地址均拒绝。
为适配独立上游的一次性播放票据，先签发三张新上游 URL，仅在本轮 Go 已停止时事务替换本轮数据库的三个 URL。
保持 source_id、desired_state 和其他表不变，严格 CAS 比对；这不是平台自动为上游签发地址，也不是复用旧票据。
使用同一 Go ELF 和参数执行 SIGTERM 正常重启，不冒充 crash 恢复。

原 helper 在停止前因 Python 3.10 不支持九位小数 RFC3339Nano 而 FAIL；已创建本轮未消费凭据，
但未发停止请求、未更新旧源 URL。原失败保留，独立 v2 使用精确整数纳秒，未放宽任何 TTL 条件。
`real-final/restart-parser-review-20261014.json` 保存离线 RED/GREEN 与前置状态核验。

`real-final/after-restart-decode-20261014-v2/result.json` 中 4/4 目标同时通过 FFmpeg 与真实 Chrome：
三种新代拉流及未消费原地址的新发布，每目标 FFmpeg 76 视频帧，音频样本分别为 142336、144384、143360、145408。
8 张独立新播放票据，保持严格 PTS、双轨解码、尺寸和错误断言；原生 Chrome samples/frames/energy 连续增长。
服务端实际提供的五个 UI 模块 SHA256 与已提交源文件逐一相同，见 `real-final/embedded-ui-check.json`。
最终清理 PASS：`real-final/final-cleanup-review.json` 中 12 个登记 PID 全部不存在，
`processes.json` 的 cleanup errors 为空，隔离媒体、signaling、SIP 与浏览器监听端口残留为 0。
退出进程不可能继续持有其 FD/socket；这不单独证明 allocator 回到初值或所有库均无内存泄漏。
独立关页补测也完成清理：`real-tab-close/evidence/own-cleanup.json` 中五个本轮身份与端口残留均为 0；
四个服务退出码为 0，FFmpeg publisher 经有界 SIGTERM 等待后被身份核对过的 SIGKILL 结束，
不是虚称全部优雅退出。原始失败没有重跑或删除。
只处理本轮 PID/starttick/exe/netns 核对过的进程；不清理其他服务、namespace、工作区或原始证据。

## 7. 复杂性与生命周期自审

- 数据库授权事实唯一；prepared 表示确认前表单期限，pushRuns 表示当前认证运行，不是凭据消费的副本。
- WHEP pending_checks 保存事务、候选和有限有效期；consent_received 是认证新鲜度，不能以媒体活动替代。
- ICE check、DTLS、建立期限 timer 有独立协议职责；没有新增 started/closed/shutdown 去重状态。
- HTTP 请求局部 active 只保护已排队 deadline 对局部变量的访问，不是会话第二份终态。
- owner worker 清 UDP 写回调和 DTLS 强回调，解除引用环；外部 DELETE 仅持 URL/token，不持会话。
- 详情 mounted 是路由所有权；播放器状态是协议状态的展示，streamingAt 过滤早于播放的快照。
- 删除旧 `playback.js` 五地址面板；保留有真实多个调用方的 player 和三协议创建认领流程。
- 未引入插件框架、第二 registry、共享 RTP/SRTP、GOP replay、无界重试或延迟关闭。

## 8. 边界与未决事项

WHEP 输入仅本轮真实验证的 HTTP、直连 UDP、ICE-Lite 上游、H.264 + 可选 Opus、BUNDLE/RTCP mux。
不承诺任意 full-ICE 角色冲突、HTTPS、TURN、Trickle、restart、NACK/RTX。
尚无 Location 无法定位远端资源；系统正在执行的 DNS 查询不保证严格 5 秒内结束。

原 `real-env/ffmpeg-seven-20261014-v3` 验收存在首音频 `0 → -95 samples → 913…`，即回退约 1.97917ms，仍保留为未决 P1。
独立 wire 观测 124 个音频 RTP timestamp 严格增长，PLAY 无 RTP-Info；源码与观测指向首 RTP 独立归零后，
首次 SR 切共同 NTP 原点的 RTSP 初始化交互。但未捕获原失败同一连接的 SR，不能声称已逐包证实根因或已修复。
独立 debug 解码和最终七目标严格测试通过不抹去原失败，也不据此改断言。
这不是音频被丢弃的假通过：Chrome 原生 samples、duration、energy 均增长，FFmpeg 实际解码有明确样本数。

随后固定三次同连接透明转发诊断，每次只签发一张新票据、运行三秒，不自动重复至成功：
`real-final/whep-same-connection-relay-20261014/` 保留 wire metadata、FFmpeg debug_ts、命令、身份与 SHA。
三次原严格解码/PTS 断言均 PASS，每次 76 视频帧，音频样本分别为 143360、144384、143360。
确认已转发前缀中的音频 AU 分别为 145、148、144，序号连续、单 SSRC、时间戳严格增长。
各次首视频 SR 在首音频 RTP 前，首音频 SR 位于该轨首个和第二个 AU 之间。
按实际 wire 事件顺序和共同 NTP 换基计算，前八个整数 PTS 与同连接 demuxer/ashowinfo 完全一致；
首两点分别为 `0→2593`、`0→672`、`0→2449`。第二次以较晚 SR 回算首 AU 得到 -336，
但这是反事实计算，不是实际解码出的负 PTS。关闭时未确认送达的尾部字节不用于该关联。
详见 `same-connection-analysis.json`；未保存真实媒体 payload，未改写 SDP/RTP/RTCP。

这些证据收窄了初始化时钟机制，却不能唯一证明旧连接的 -95 只由同一机制造成。
旧失败没有同连接 RTP/SR，不能将其改判为合法差异或已修复；也没有证据据此修改 RTSP 生产实现。
临时 relay、FFmpeg 进程均已退出，原始失败、脚本和诊断保留。

## 9. 功能状态与整合决定

| 验收项 | 当前结论 |
| --- | --- |
| GB 通道失败循环、300 秒预览/确认/持久/消费/重启、原播放 60 秒 | PASS，Go 与真实浏览器/重启分别验证 |
| RTSP 自动拉流、RTMP 主动拉流、WHEP 主动拉流 | 核心真实双轨、浏览器、重启与错误/关闭路径 PASS；WHEP→RTSP 首音频 PTS 边界未决 |
| RTMP/RTSP/WHIP 发布、合成 GB 输入 | PASS，真实双轨 FFmpeg 与 Chrome |
| 详情播放、单/四画面、快速替换、路由退出、三 viewer 共享上游 | PASS，当前 UI ELF 已校验 |
| 真实关闭 Chrome 标签页 | 真实关闭与匹配的后端清理通过；补测整体 FAIL，DELETE 网络观测缺口保留 |
| 完整 Normal CTest、Go test/vet/race、UBSan 定向 | PASS |
| 完整 ASan CTest | FAIL 65/67，两个环境故障边界 BLOCKED；新增输入受测路径 PASS |
| Firefox 真实 H.264 | BLOCKED，浏览器能力缺失 |

**当前整合结论：NO-GO。** 不是否认已通过的功能，而是按照“未决 P1 不可宣布 GO”的验收约束，
暂不建议整合 main。下一步应独立审查首 SR/初始 RTP 的时间映射；若继续复现，应在失败同一连接
保留首 RTP、首 SR 与整数解码 PTS，建立稳定 RED 后才做最小修复。
关页 DELETE 还需脱离已经销毁的 page-target 观察路径核实，不能把 context 空记录换算为通过或服务端缺陷。
不以新增缓存、延迟关闭、重试或放宽 PTS 断言换取通过，不扩大为性能优化。

本轮所有已提交改动保留在 `feature/web-console`。main、origin/main 和 third 未修改；无 push、merge 或 rebase。
