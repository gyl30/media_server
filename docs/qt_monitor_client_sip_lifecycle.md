# Qt 后续回放开发前：国标 INVITE 取消与 ACK 生命周期

基线 `47618b3d2e8211b06424d5ee4a85ede9d1f8c1e3`，仅在 `feature/qt-monitor-client` 工作区开发。审查独立录像回放的前置 SIP 生命周期时，先在既有实时路径发现并复现了缺陷，本子集先修复该真实问题；不增加历史回放接口，也不把录像目录查询标记为可播放。

## 已确认的缺陷及依赖边界

项目锁定 `github.com/emiago/sipgo v1.6.0`，没有修改依赖。

- `dialog_client.go:240` 的 `WaitAnswer()` 在 ctx 取消后同步进入 `inviteCancel()`；等待 CANCEL 的结果期间不再正常消费 INVITE 成功响应。CANCEL 的 200、481 或未答与原 INVITE 最终 200 可以交错，旧平台会清理本地 receiver，却没有正确对设备 ACK、BYE。
- `WriteAck()` 中保存的重传 ACK 经 `DialogClientSession.WriteRequest()` 再次构造，CSeq 会受对话内 BYE 的递增影响，已有 Route 也可能反复追加。正式旧生产 RED 验证了 BYE 等待期间重传 INVITE 200 得到错误 ACK CSeq，以及连续重传时 Route 累积。
- `WaitAnswer()` 原有“超过十个响应”保护会拒绝持续合法 provisional；依赖的 SIP 事务计时器也不等同应用的绝对邀请期限。本次明确改变该行为：超过十个 provisional 不再失败，仍受应用绝对期限约束。

默认 `ClientRequestBuild()` 只补缺失的 Via/CSeq，不把它与 `ClientRequestRegisterBuild()` 的递增逻辑混淆。原错误来源是对话构造路径及等待所有权，而不是默认请求构造器。不能根据最终告警或代码名称推断设备自行关闭或 UDP 连接已经失效。

## 最小修复及所有权

`live_session.go` 使用公开 `NewClientTransaction()`、`OnRetransmission()`、`Init()` 和事务 channel。同一个 start goroutine 同时消费 INVITE、CANCEL 和绝对期限；CANCEL 最终答复不决定原 INVITE 是否成功。收到首个 provisional 前不发送 CANCEL，取消后保留现有 `byeTimeout` 长度的有限补偿窗口；CANCEL 每次至多创建一个事务，合法的 UDP 重传不算第二次创建。

补偿窗口内收到合法 2xx 时先 ACK；调用已取消、Answer 无效或后续创建失败则独立 BYE 并清理 receiver。正常 Answer 继续沿用 SSRC 更新、媒体源身份、共享实时会话和一次性播放授权语义。非 2xx 由事务层 ACK，不发 BYE。

在 `Init()` 之前安装 2xx 重传回调。回调仅保留不可变 INVITE 克隆、SIP client 和 logger，不保留 liveSession、媒体 receiver 或 Qt 对象。初始 ACK、重传 ACK、BYE 复用 `live_dialog.go` 中唯一的窄请求构造函数；每次生成新请求，ACK 固定原 CSeq，BYE 使用原序号加一。当前对话只有 ACK/BYE；未来若增加 INFO，须由同一个 owner 分配对话内序号，不能并行递增。

构造保留原最终 Contact、Record-Route 反序、严格路由分支、RewriteContact 的响应来源及传输地址行为。路由对照只是与当前锁定依赖的旧构造保持一致，不声明修复了任意代理或严格路由兼容性。

`liveSession` 用 `invite`、`answer` 替代原 DialogClientSession，不新增持久字段、独立对话 registry、goroutine、原子关闭标志或 token 状态。远端 BYE 仍验证 Call-ID、tags、注册来源及当前会话指针，在锁内进入原 stopping 状态，再回复 200 并使用原清理路径。Dialog ID 从 answer 推导，不重复保存。

尚无最终答复的 INVITE 在失败出口 Terminate；已收到最终响应的事务保留默认 Timer M/D 尾期，避免本地会话清理后不能 ACK 合法重传。CANCEL 事务在有限补偿结束时清理。公开 API 实验实际观察 Timer M/D 自然结束各约32秒，不以修改全局 timer 或提前 Terminate 获得通过。

## 验证

新增五项顶层 Go 测试覆盖七种取消/成功/重传/RR组合、四种路由的 ACK/BYE 构造对照、三种绝对期限、首1xx前取消及487重传、12个provisional后成功，以及远端BYE后迟到200的ACK。最终定向 race 实际通过，14.986秒：七种组合都完成receiver/session/SSRC归零；期限200ms加补偿100ms的三场景约301ms返回，随后跨过真实Timer A继续观察，无INVITE重传；仅收到provisional时只有一个CANCEL事务，完全无响应时零CANCEL。两种487场景各有两个原Via/CSeq ACK、零BYE；连续12个180后收到两份200则两个原CSeq ACK、一次显式停止BYE。远端BYE回200、清理后重传200仍获原CSeq ACK，无反向BYE。

最终版本 Go 全量 test 实际通过：signaling108项顶层、81.792秒；simulator6项、0.002秒，共114项，无失败或跳过。命令使用 `GOMAXPROCS=2 go test -p 1 -count=1 -json ./...`，保留 JSONL、stderr 和退出码，不把只跑定向测试当作全量回归。

同版本全量 race 实际通过：signaling108项、86.733秒；simulator6项、1.011秒，无失败、跳过或 DATA RACE；`GOMAXPROCS=2 go vet ./...` 退出0。本次没有 C++/桌面修改，不重跑或冒领其历史 sanitizer 结果；组合验证使用保留且校验SHA的真实媒体/Qt ELF与本次新构建signaling。

正式 UDP 测试的媒体 HTTP 边界使用 mock，能验证真实 SIP 报文、期限及清理，不是 RTP 或物理摄像机验收。路由测试仅截获公开构造器，不假称真实代理网络。

另一次冷 GB 四画面组合使用实际 signaling、media_server、GB 模拟器和 Qt FFmpeg 解码器。四个独立票据各消费一次，四画面各解码 H.264 251帧和 PCMA 274帧，PCM 各525984样本；停三留一后3秒内最后一路增加75视频帧与75音频帧。四画面关闭后模拟器仍只有1次 INVITE、1次 ACK、0次 BYE、1路 live；显式删除 live 后收到 BYE，receiver 消失。四个自有进程正常退出0且 reap。这里使用320×240/25fps及虚拟显示/音频，证明实际双轨解码和共享上游，不是16路容量、物理扬声器或真实摄像机。

## 保留的失败、限制与后续

- 旧生产 `formal-cancel-red-03.log`、`formal-cancel-red-race02.log` 以及 `formal-route-red-03.log` 保留。早期测试 fixture 的错误断言、重复 BYE UDP 数据报计数及过宽 mock 已分别修正，不掩盖原始失败。
- `formal-protocol-green-01.log` 实际 FAIL：测试只等1秒，没有覆盖依赖 `sip/transaction_client_tx_fsm.go:342` 的 `actAckResend()` 明确等待默认 T2=4秒的行为。首个487 ACK继续限1秒；重复487 ACK单独观察 T2+1秒，未改变全局timer或生产逻辑。保留依赖的 `ACK loop retransimission` ERROR，不把非2xx重复ACK声称为立即发送，也不把这个既有依赖边界冒称本次新增缺陷。
- 补偿窗口外才到达的首次 2xx 已无法由这个结束的 INVITE 事务 ACK；对端如何最终停止尚未验证，不能声称无任何远端残留。
- UDP 写操作本身不受 ctx 中断；停机期间 ACK/CANCEL/BYE 发送是尽力而为。应用总期限不应解释成操作系统调度和网络 syscall 的硬实时保证。
- Timer M/D 尾期约32秒仍可保留 INVITE/client，这不是 liveSession 或媒体资源泄漏证明。测试 UA 关闭后还会出现 sipgo UDP 负引用计数 warning；原始日志保留，未完成依赖内部归因，不宣称已证明无害。
- 无最终 To-tag 的不合法 2xx 仍失败，未改变旧路径的格式错误边界。
- 首 ACK 与重传的所有可能内核调度交错不能靠测试固定；生产上用 Init 前安装回调保证没有安装窗口，不新增测试专用接口。
- 只修改 Go SIP 控制路径；桌面 FFmpeg/Qt、媒体服务器、第三方、录像目录格式及真实摄像机配置均未改。历史回放仍需独立会话、媒体首尾与控制协议设计，不能直接复用实时 viewer 所有权。

证据：DEV `/var/tmp/media-server-qt-history-preflight-owjdYWMd/`；公开事务尾期实验 `/var/tmp/media-server-qt-public-sip-probe-nd8iyIMY/`；Claude 第31–33轮讨论 `/tmp/qt-client-plan-claude-JuZHjy/`。原始日志可能含完整地址、授权及 SIP，不提交或公开复制。

Claude第34轮最终复审无P0/P1；其提交前要求的全量race/vet随后均已真实通过。按阶段清理要求检查本轮产物：Go自动清理自身临时编译目录，本轮没有需要手工删除的大型中间构建；原始RED/GREEN、源快照、协议标准、ELF与SHA清单全部保留，未清理历史记录、他人进程/目录或系统依赖。
