# 阶段六：报警通知与桌面近期列表

## 范围和接手基线

接手基线 `59bd0cbc66a15527bb1912e0f7a1628d434cb89f`，工作区 `/home/gyl/example/media_server_qt`、分支 `feature/qt-monitor-client`。保留 Claude 已写的报警半成品，在其基础上修正游标与取消边界，不重做阶段五。按用户要求停止了原 Claude 会话和额度恢复续接任务；本增量由 Codex 实施、独立代理只读复审，不把额度中断当作审查批准。

本增量只接收已注册设备主动上报的报警、返回协议响应并显示近期记录，不包括报警订阅、复位、持久化、广播或对讲。整阶段六和完整平台仍未完成。

## 信令行为

- 复用既有 MESSAGE/XML 入口和 64 KiB 报文上限。只接受 `Notify/Alarm`、有效 SN 和 20 位报警源编号；父注册设备可以上报自己或当前已知所属通道。来源 endpoint、在线、注册期限及 stopping 校验沿用已有事实，不因报警刷新心跳。
- 返回 SIP 200 后发送独立的 `Response/Alarm` MESSAGE，保留原 SN、报警源 DeviceID、`Result=OK`。失败沿现有期限和日志报告，不增加重试队列。GB/T 28181—2016 §9.4.2 及附录 J.8 的响应示例支持此流程；不要把响应 DeviceID 换成平台编号。
- 近期缓存只在内存中保留 1000 条，每页最多 200 条，重启清空，不写数据库。
- `GET /api/alarms/since/0` 首读；随后原样传回响应的 `next_cursor`，路径为 `/api/alarms/since/<cursor>`。游标由运行实例身份和本页最后返回的序号组成，不是凭据。响应包含 `alarms/latest/next_cursor/reset/gap`；`latest` 只表示水位，不能用它推进客户端。
- 平台重启时，实例身份不同即 `reset=true` 并返回新实例第一页，即使新报警数量已经等于或超过旧序号。初始空页也返回实例游标。容量淘汰未读记录时返回 `gap=true`，不承诺无损事件队列。非法游标返回 400。

## Qt 行为与所有权

报警页激活时每 2 秒查询一次，一次最多一个在途请求、5 秒超时。离开该页停止新增查询；已有请求允许完成。列表按接收顺序倒序显示，最多 500 条，平台淘汰未读记录时明确提示。

Qt 只保存一份不透明字符串游标，不复制 sequence 或平台 generation。成功响应后取 `next_cursor`；错误或缺少游标时保留旧列表与游标，等待下一次既定轮询，不递归重试。

请求使用现有 `listing_` QObject context。刷新或切平台先销毁旧 context，随后 abort 并清空旧 reply，再从 `0` 开始；旧回调不能修改新列表、清空新请求或阻塞新平台。窗口关闭继续沿既有 context/网络 manager 生命周期收尾。

新增运行事实仅为平台报警缓存及其互斥保护、实例身份/序号，以及桌面 timer/在途 reply/单一游标；无 DB 字段、第二套 registry、关闭标志、后台媒体任务或新通用框架。

## 缺陷与 RED/GREEN

证据根：DEV `/var/tmp/qt-codex-resume-tRclw0/`。原始日志可能包含本地测试授权，不能公开提交。

1. 重启后仅凭 `after > latest` 判断会静默漏报：旧游标 100，新实例已有 100 条时返回空，已有 150 条时仅返回 101–150。`alarm-go-restart-red.log` 保留原失败；加入实例游标后追平/超过/空实例回归、分页与容量缺口、并发读写通过，见 `alarm-go-directed-02.log`、`alarm-go-race-final.log`。
2. Qt 旧请求在切平台后仍留到超时：`qt-red-02.log` 在实际请求建立屏障后确认旧 reply 未结束；context 销毁后显式 abort，`qt-green.log` 通过。`qt-red.log` 同时保留初版测试未等待设备初始化的夹具时序失败，不把它算作生产 RED。
3. `qt-directed-normal.log` 覆盖实际返回页游标、异代重置、503、单在途请求、500 条上限、淘汰提示和缺少游标不推进。新增的有界定时器测试使窗口测试总时长增加，CTest 预算由 60 秒调整为 90 秒；未放宽单请求期限。

## 验证

- 完整桌面 Normal 与 ASan+UBSan 各 7/7 CTest、396 项 QTest，无失败/跳过，229.00/228.41 秒。原始结果为 `qt-final-ctest-{normal,asan}.log`，完整子测试输出另存 `qt-{normal,asan}-LastTest.log`。使用已有独立桌面构建及新源码重编，不声称新建了正常服务器 SDK；RelWithDebInfo 含 `-O2 -g -DNDEBUG -Werror`，sanitizer 编译额外带 `-fsanitize=address,undefined -fno-omit-frame-pointer`，实际链接 libasan/libubsan，泄漏检测开启、无 suppression。
- Go 全量 `go test -count=1 ./...`（主模块 116.163 秒）、`go test -race -count=1 ./...`（122.617 秒）和 `go vet ./...` 均退出 0，见 `go-full.log`、`go-race.log`、`go-vet.log`。本机实际 Go 为 1.27.2，go.mod 最低 1.26；GCC 为 16.0.1 experimental，不写成 16.2。
- 报警 Go 定向测试覆盖重启追平/超越、初始空页与空实例重启、非法游标、分页/淘汰、并发访问，以及实际 UDP SIP 下错误来源、未知通道/设备、离线、到期、stopping 和不刷新心跳。只接收两条合法通知，响应 SN/报警源身份与缓存均核验。
- 未修改媒体服务器 C++ 生产实现，因此未将旧服务器 69 项 CTest 冒充本增量的新执行结果；本轮 C++ 全量指桌面 7 项。

实际报警组合在 `alarm-integration/`：生产 Go HTTP handler、实际 UDP SIP peer 和实际 Qt `monitor_window`。normal 与 ASan+UBSan 各 1 项通过（11.214/11.680 秒，Qt 退出 0）；每组 3 次 Notify、3 次独立 Response MESSAGE，平台 A 两行、切到 B 后仅一行；切换取消旧 HTTP，隐藏/销毁后不再轮询。不是 mock HTTP 报警数据，也不是物理设备验收。ASan 组合启用泄漏检测，未设置 suppression；Qt/SDL/FFmpeg 等依赖没有全部插桩。

## 明确未完成的边界

- 物理摄像机报警触发、订阅/复位、持久化与高频无损采集未验收/未实现。
- seek 和回放倍速未实现；当前有限 PS 通道不传递 seek 边界，不能以重开会话或猜 PTS 冒充同会话 seek。
- 广播/对讲未实现。现有 PS sender 要求视频，不是纯音频上行；还需明确设备支持、音频采集/编码、G.711 RTP 方向及实际设备验证。不能用 SIP 200 或现有观看音频替代。

本报警子集具备合并审查条件，不据此宣布完整客户端 GO。
