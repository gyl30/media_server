# 广播内部 SIP 控制 owner

本阶段基于 `3a40290`，实现媒体路径无关的内部广播控制。没有新增公开 HTTP 接口、客户端租约、Qt 采集或广播 UI；直送与中继仍按后续媒体验收决定。内部控制成功不代表相机收到音频、解码或扬声器出声，完整阶段六及平台仍未 GO。

## 控制与所有权

`liveService.startBroadcast(ctx, deviceID, local)` 只接受注册主设备、单路 UDP/PCMA、IPv4 直连 profile。调用方先绑定 RTP/RTCP 端口，持有父 ctx 覆盖整个媒体寿命，并监听返回的会话 ctx：远端 BYE、设备下线、删除及停机会取消该 ctx，媒体调用方须立即停止采集与发送。内部 owner 不持有媒体 socket，不把它当实时视频 receiver。

- 每个设备同刻只占用一个广播 owner，复用 `live.mu` 和既有设备停止 fence。
- SQLite 按 SIP 域保存七位序号高水位，生成 `10位域 + 136 + 7位序号`。序号1预留给历史私有探针；分配提交后才发送 Notify，失败也不回收，耗尽直接拒绝。
- Notify 的 SIP 2xx、独立同 SN 的 `Broadcast Result=OK`、反向 INVITE 的 200/真实 ACK 分别验证；三者齐备才返回媒体端点。业务响应与 INVITE 可乱序。
- MESSAGE 沿用统一 XML/MIME/大小入口，核对根、CmdType、SN、目标主设备、From、当前注册来源及 owner。旧 SN 不影响新 owner。
- INVITE 核对 Request-URI/To 的本次 SourceID、From/Subject 双方身份、注册来源、SDP 和大小。只协商 PCMA/8000/mono PT8、recvonly；媒体与 RTCP 目的 IP 必须等于该注册来源 IP，支持显式 RTCP 或相邻端口。没有 `y=` 时分配独立非零 SSRC。
- 统一 INVITE/ACK/BYE 路由，保留原事务 `tx.Acks()` 与顶层 `OnAck` 两条真实路由；调用 sipgo `ReadAck` 前核对完整 UAS dialog、SourceID、来源及终态。广播 BYE 先按自身 dialog 分派，其余交给既有 live/history owner。
- INVITE handler 持有原事务直到应答完成；取消时 Terminate 并 join 应答及 ACK 工作，之后才读已发布的 InviteResponse 或发送 BYE。sipgo v1.6.0 的应答循环不监听业务 ctx/tx.Done，终止后仍可能提交一次既有200，再从计时重传返回事务错误；没有把 Close 写成取消屏障。
- 远端 BYE 按远端结束清理；本地停止与对端确认分别保留。owner 清理结束后按 identity 移除，设备停止 fence 等待清理。删除 HTTP 取消不提前撤 fence；SIP/媒体清理仍使用各自既有期限。

持久高水位只保证**同一未回退数据库**内不复用。复制数据库后同时运行、回滚备份、重建或手工改计数都不具备跨实例唯一保证；不得把新空数据库部署到仍可能收到旧会话请求的同一 SIP 域来宣称身份连续。私有实机 V2 沿用 V1 数据库，已分配2/3后继续4/5，没有重新建库。这里没有引入跨库分配服务或迁移兼容层。

## 自动验证与真实修复

真实 UDP 测试覆盖业务响应/INVITE 乱序、Notify 2xx 门槛、同 branch/新 branch ACK、错误 CSeq/dialog、旧 SN/SourceID、并发占用、Notify/业务拒绝，以及取消、远端 BYE、下线、删除和停机。SDP 表测试覆盖18个合法/异常 profile；SQLite 测试覆盖并发分配、关闭重开及耗尽不回绕。

删除取消新增回归实际观察到 RED：BYE 尚未确认时 `fenced=false, owner=true`。改用 `context.WithoutCancel(ctx)` 等待既有有界清理后 GREEN，数据库删除仍使用原请求 ctx。没有新加关闭标志或设备停止 map。

首轮取消测试未考虑 sipgo 的一次既有200重传而失败，保留 `broadcast-lifecycle-01.log`。修正观测只接纳同一对话的既有200，随后仍严格要求 BYE/确认、owner 归零；不改变生产或音频门槛。编译准备时发现的未用 import、HeaderParams 临时值调用也已修正，不把编译失败称为行为 RED。

最终 `go test -count=1 ./...` 通过（122.04秒），`go test -race -count=1 ./...` 通过（129.62秒），`go vet ./...` 通过（0.35秒），包含主模块及 simulator。最终8个Go源码/测试文件均与受测 SHA 一致。桌面和媒体服务器未修改，本阶段未重跑其 CTest 或 ASan/UBSan，不继承此前结果。

## 大华控制实机

DEV 证据 `/var/tmp/qt-broadcast-owner-3a40290-9jiged1c/`；TEST `/var/tmp/qt-broadcast-owner-3a40290-CRzxRwI3/`。凭据仅由进程读取交接指定0600文件，未进入 argv、源码或日志；配置只读，音量 GET 仍为97，平台保持 `172.20.63.32:15060`。

| 版本 | 实际范围与结果 |
| --- | --- |
| V1 | 同一 SIP 进程约22秒完成注册/目录及两次内部控制，SourceID尾号2/3；正常BYE、owner0、PID消失及15060/38442/38443重绑定。未包含后续媒体结束ctx修正，不算最终代码实机通过 |
| V2 | 修正后精确源码，沿用同一DB，等待真实注册/目录后在同一SIP进程连续完成尾号4/5两次控制；约88.57秒，正常BYE、owner0、PID消失及三个端口重绑定。没有音频发送 |

初始注册观察预算按设备3600秒 REGISTER 周期预先固定65分钟；Broadcast 本身仍使用既有10秒协商预算。V2实际约81秒就绪，没有重启空平台充当同一次音量实验的下一轮，也没有伪造在线、改注册规则或 SET 相机配置。REGISTER、Catalog 和 Keepalive 分别记录，避免此前无法区分就绪事实的问题。

V2在TEST实际执行，DEV/TEST的ELF SHA256均为 `8591176290e81248e2b13ab050633c88a2d08418a48d7e1ee2512d14dcfe2259`。它绑定 `probe-build-source-v2.txt` 中的 `3a40290 + dirty` 源码与独立 overlay，不称为提交后 clean 构建。

本轮实机只是生产内部 SIP 控制验收，没有发送 PCMA/RTP，没有验证 Qt、租约、跨 IP 媒体来源、相机收包/解码或声音。旧音量对照仍为 INCOMPLETE、旧关联分析仍为 INCONCLUSIVE。DEV可枚举ALC897采集硬件，Pulse当前只有monitor，均不证明已经连接实物麦克风。

## 只读复审与后续

独立规范审查未发现明确违反；事实审查要求明示数据库灾备边界，并建议补错误dialog及发送失败测试，已处理。DEV Claude 首轮长输入180秒超时，无结论；短复审发现媒体调用方缺少结束通知的 P1，返回会话ctx并补回归后，最终只读复审未发现 P0/P1。复审范围、限制及原输出均保留。

尚未覆盖的注册来源迁移不宣称支持。锁内 SQLite 查询、正常 HTTP 停机先排空入口等沿用当前职责；后续接入公开接口时须验证租约、201写失败补偿、媒体端点来源与停止通知，不能直接把长寿命媒体绑定创建请求 ctx。Qt采集/发送、完整对讲、真实听音、第二机实际媒体及公共分发许可仍待完成。
