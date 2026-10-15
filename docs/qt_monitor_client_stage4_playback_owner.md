# 阶段四：国标历史回放的信令 owner

本增量接续 `7375848` 的独占有限 TCP/RTP-PS 通道。它只实现历史控制与传输接续，不将解码探针称作 Qt 界面回放，也不是阶段四或整个客户端的最终验收。物理设备录像、后续播放控制、下载和后续阶段仍未完成。

## 控制接口

`POST /api/devices/{device_id}/channels/{channel_id}/playback` 接受 JSON `start_time`、`end_time`，均为 UNIX 秒；起点必须大于零，终点严格晚于起点。入口一次校验格式及 `time.Duration` 的可表示范围，不人为增加一天等录像长度限制。设备必须已配置并在线、通道必须存在；不要求通道实时状态为 ON，因为录像可以来自当前离线的摄像通道。

201 包含 `playback_id`、独占 `ps_url`、票据的 `expires_at` 及本次资源预算 `deadline`。历史实例使用独立 UUID，同一设备通道的多个历史实例不互相复用，也不占用实时 canonical key。普通 `/api/play` 不能为它续签，普通 `/api/lives/{id}` 不能删除它。

`DELETE /api/playbacks/{playback_id}` 终止指定实例；成功 204，不存在 404，清理失败 502。它不表示媒体 socket 已同步完成关闭，沿用既有异步删除语义。

参数错误 400，设备/通道不存在 404，设备离线或正在停止 409，准备失败 502。创建结果不明且补偿也失败时，502 才额外带 `playback_id`，允许显式 DELETE 重试；确认已回收的失败不返回失效资源 ID，更不签发媒体票据。

## 授权与建立顺序

1. 后台 owner 在独立 context 中准备 receiver，复用原有“创建结果不明”补偿逻辑。历史请求仅四字段，`tcp_passive`，读取真实响应 `rtp_port`；普通 UDP 的偶数校验不套在 TCP 返回端口上。
2. 准备成功后，锁内保存端口并签发唯一票据。默认 60 秒，不改变被动推流的 300 秒凭据规则，也不写入数据库。
3. 取得票据还不会发 INVITE。媒体 GET 经真实 signaling `/internal/verify` 成功时，在现有锁和 token map 内永久删除票据并唤醒 owner，验证响应不等待 SIP 事务。后续失败不恢复票据。
4. owner 复查实例、取消与设备/通道事实，再发 Playback INVITE，TCP passive/new、`u=channel:0`、UNIX 时间、历史 SSRC 首位 1。`u` 的尾参数 0 是当前实现约定，不声称标准强制所有设备如此。
5. 复用实时 INVITE 的 ACK、取消补偿、成功响应重传和设备自选 SSRC 更新路径。历史应答另要求 TCP active/new。实时 SDP 的字节回归保留。

SSRC 仍来自同一末四位唯一池；历史和实时只改变首位，不扩展成两个可能冲突的池。释放使用原始分配值的后四位，而非设备在应答里自选的值。

创建 HTTP 取消或写响应失败，经既有 stop 路径取消 owner；不是 HTTP handler 和后台任务同时清理同一个 receiver。未 GET 的票据到期，由 owner 删除已准备的 receiver。票据消费是授权事实，不是媒体消费者已经完成绑定或录像已成功播放的证明。

## 自然结束与有界清理

历史 `MediaStatus/121` 与远端 BYE 需要完整 dialog 及当前注册来源；121 的 XML DeviceID 必须是对应通道。ACK 后设备可能在 SSRC 更新 HTTP 尚未完成时通知结束：先响应 200，再由当前 SIP handler 等现有 `established`。失败建立已由原 owner 清理，不被结束通知改写为成功；只有同一个实例已经 Streaming 时，才唯一进入 Stopping。

121 触发本平台 BYE；远端 BYE 不反向再发 BYE。独立收尾任务保留最多 20 秒，等待原始 TCP 尾部。既有 5 秒媒体对账若先发现 receiver 消失，就取消收尾等待并完成清理。DELETE、设备退出和进程停止也直接取消等待，不增加另一份完成/关闭标志。

20 秒到期按传输可能不完整记录，并终止 receiver；SIP 200、121、BYE、合法 TCP FIN 和 HTTP EOF 都不单独证明录像内容完整。UI 后续还必须区分解码结束与尾帧/音频呈现结束。

1x 子集另外采用“请求录像时长 + 未认领票据 TTL + 60 秒应用宽限”的绝对资源预算。Preparing/Inviting 由 owner context 响应，Stopping 由收尾任务响应；已 Streaming 的过期实例在既有对账查询远端之前请求终止。正常调度下还会有约一轮对账及实际清理操作的延迟，不能宣传为进程冻结或网络异常时的硬实时回收保证。后续增加 PAUSE/变速时必须重新核对这个预算，不能悄悄沿用。

CleanupPending 不因到期而忘记：BYE 发出不等于设备确认停送，删除失败也不能视为 receiver 已回收。继续持有实际实例及 SSRC，保留显式 DELETE、设备退出和停机的现有重试入口，不新增自动重试。媒体服务不可达时不能保证立即归还其实际端口。

## 最小状态与所有权

- 新增 `liveKey.historyID`，区分同通道的独立历史实例；不增加第二套会话注册表。
- 新增 `liveSession.claimed` 通道，只表达后台 owner 的一次认领事件；消费事实只在现有 token map 中。
- 保存与原 `cancel` 配对的 `session.ctx`，供后台等待和收尾判断取消；不是新的关闭状态。
- `prepareReceiver`、INVITE、BYE 和 dialog 查找在两个真实业务调用点复用，保留原错误与补偿路径，没有一调用包装或通用输入工厂。
- 停机先排空 HTTP，再关闭会话 owner，最后停 SIP，使后台历史事务仍可执行 CANCEL/BYE。未消费的实例不等待完整票据 TTL。

## 证据与验证边界

定向基础证据：DEV `/var/tmp/media-server-qt-history-foundation-1g5AaGey/`。结束交错测试使用真实 UDP SIP 与受控媒体 HTTP 边界，不是实际 PS 解码。

已有三组 ACK 后 SSRC 更新屏障场景：121、远端 BYE、121 后更新失败；检查错误 tag/Call-ID/通道和错误 NotifyType，正常路径不提前删除 receiver，失败路径保留原始建立失败，最终会话/SSRC/receiver 控制记录归零。race 三次重复共九子场景通过。新测试最初遗漏 import 和等待 BYE 应答的编排错误已修正，失败输出保留；不称为生产缺陷 RED/GREEN。

本次无数据库迁移，也不恢复重启前的历史会话。Qt 有限呈现、录像控制 INFO、物理 NVR、不同设备的 121 dialog/RTCP 复用/FIN 习惯、请求范围内容完整性都不从这些控制测试推导 PASS。

### 当前增量的实际验证

- `owner-final-full-go-02.log`：当前源码 `go test -p 1 -count=1 ./...`，主包 106.176 秒及模拟器包通过；`owner-final-full-race-01.log`：`go test -race -p 1 -count=1 ./...`，主包 111.205 秒及模拟器包 1.009 秒通过；`owner-final-vet-02.log` 退出 0。均包含下述独立推流错误返回修复，不自动继承早一版结果。
- C++ 动态端口增量补跑完整 ASan+UBSan CTest：`/var/tmp/media-server-qt-finite-ps-kLBXkM/logs/pool-asan-full-ctest-01.log`，69/69、459.02 秒、退出 0，LSan 开启；ELF 与冻结端口增量一致。本次 Go 改动不改 C++ 源码，不把它说成新增 Go 代码的 ASan 插桩；详见传输子集的原始输出和 SHA。
- `gb_playback_test.go`：参数、票据准备前不 INVITE、8 次并发认领仅一次、同通道历史实例与实时实例独立、票据过期、准备失败补偿、绝对预算、真实 HTTP 取消、未认领实例停机及认领/DELETE 交错。媒体控制端为受控 HTTP，不冒称实际媒体测试。
- `gb_playback_end_test.go`：真实 UDP SIP 的 ACK 后 SSRC 更新屏障，覆盖 121、远端 BYE、更新失败；另覆盖双结束请求竞争及 DELETE、设备离线、停机、媒体对账和实际 20 秒收尾定时器。`drain-race-01.log` 退出 0、22.080 秒；不能把真实定时器测试写成可控时钟测试。
- `TestPlaybackCreatedResponseWriteFailure` 在写入 201 JSON 时注入错误，断言原 owner 回收且不 INVITE；它不是实际 TCP 对端断连，真实 TCP 取消由另一个 HTTP 用例验证。
- `/var/tmp/media-server-qt-history-media-L8h9VQig/run-04/`：四组真实 Go HTTP/认领、实际 UDP SIP、冻结的 normal/ASan C++ 接收器和 QtNetwork customAVIO/FFmpeg 解码组件。每组 50 帧全部 luma 指纹一致，AAC 97280、PCMA 16000 样本及 PCM hash 一致；INVITE/ACK/BYE 各 1、验证 1，最终 owner/票据/SSRC/receiver 均 0，server/decoder 退出 0。此轮在独立推流凭据修复前，不能称为修复后新源码验收。

上述组合使用私有 Go overlay 与受控 SIP 设备，预置设备注册事实，没有测试 REGISTER 或真实 NVR。Qt 与外部 FFmpeg shared 库不是完整插桩；ASan 组的项目 C++ 与解码探针启用 ASan/UBSan/LSan，未关闭 leak detection。依赖的 UDP 引用计数警告和 makecontext 警告保留，不称日志完全无警告。原始 JSON/日志可能包含短期凭据，留在私有证据目录，不公开提交。

独立推流错误返回修复后的最终 Go 源码清单 SHA 为 `b2fbbf62198b6143220f96beac1bb8a9f9a4c6e2a98092acd4326b16e5b3fb7e`，媒体回归前后不变。新增 `run-05/` 四组全部通过，Go 用时 1.038 秒，同样逐帧/PCM 精确核对与资源归零；不是复用 run04 的旧结果。

同一源码还复用实时冷首开四格回归：实际 REGISTER/Catalog、Qt `monitor_window`、H264+PCMA。`/var/tmp/media-server-qt-current-realtime-HAaleJjC/quad-01/` 的 14 个编排检查及 Qt 193 项断言全通过，实际 21225 毫秒。四张独立票据、四次 DESCRIBE，共享一个 INVITE/ACK；停三格后剩余画面继续增加 77 个视频及 77 个音频帧，三个 decoder 已析构；全部 viewer 退出仍不 BYE，显式停止 live 才 BYE。四个本轮进程全部退出 0 并 Wait 回收。该回归使用 offscreen/dummy 音频，不冒充物理显示、扬声器、真实设备或容量验收。完整私有汇总为同目录 `current-realtime-history-review.md`，SHA `67ea4ce253502f265458e2966c95736d383000a23a4ed9f46787444891bd663e`。

### 全量回归发现的独立既有缺陷

`owner-final-full-go-01.log` 首次最终全量失败，定位到既有 `TestPushCredentialConcurrentConsumptionAcrossConnections`。未修改的原函数/原测试独立 30 次中 7 次失败；增加诊断的私有 overlay 30 次中 6 次失败。另持有独立 SQLite 读事务，3 次均稳定复现：`UPDATE ... RETURNING` 的 Scan 先填入设备 ID，随后 Rows.Close/finalize 提交失败返回 `SQLITE_BUSY`，原函数返回了非空 ID 和错误。

现有认证入口先判断错误，未将这类 ID 当成授权成功。最小修复仅使任何非 nil 错误返回空 ID，保留原错误、原子 SQL、无记录语义和忙锁策略，不添加重试或锁。新增持锁测试释放读事务后验证仍可消费一次，随后不可重放。修改后原并发用例和持锁用例各 30 次通过，`fixed-directed-01.log` 退出 0、10.279 秒。此修复与历史功能分别审查、分别提交；不以重跑偶然通过掩盖原失败。

独立 RED/GREEN 原始证据：`/var/tmp/media-server-push-busy-diagnostic-fG5YLUDm/`，含未修改的源码 SHA、私有 overlay、原错误、确定性 RED 以及修复后 GREEN。最终是否已提交以 Git 为准，本文不以待提交代码冒充已发布版本。

### 提交前复审

原 Claude 第 51/52 轮因额度限制未完成，不作为批准。随后本机 Codex 0.162.1 在只读沙箱独立复审两轮：首轮两个逻辑均无 P0/P1；追问轮逐条核对 121 在 SSRC 更新期间阻塞的是独立 SIP handler（sipgo 每请求独立 goroutine，最长约更新剩余超时+BYE+删除超时，默认约 9 秒），认领与 DELETE 交错时 owner 在锁内复查取消而放弃 INVITE，单票不变量保证 `claimed` 只关闭一次，对账唤醒 drain 由原 owner 清理，设备自选 SSRC 不改写 `session.ssrc`，201 写出由 net/http 在 handler 返回后刷出、客户端读完再断开不会触发补偿。删除失败时新 `done` 保持打开表示 CleanupPending 仍持有资源，不是等待泄漏。原始问答保存在 DEV `/var/tmp/qt-claude-owner-work/codex-review/go-round{1,2}.*`；讨论不替代上文测试。
