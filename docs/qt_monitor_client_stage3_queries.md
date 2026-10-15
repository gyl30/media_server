# Qt 阶段三：设备信息与状态查询子集

## 范围与当前结论

本子集以 `7ea96661686ee40b910cec07f7a31b536a82e2d9` 为基线，在 `feature/qt-monitor-client` 实现 DeviceInfo、DeviceStatus。它不是整个阶段三完成：PTZ、预置位尚未实现，真实可动云台及录像设备的可用条件待确认。不能将受控 SIP peer 的响应等同于摄像机物理动作验收。

播放器仍为 FFmpeg 解码、Qt Widgets 显示、SDL2 PCM，使用现有 signaling 和 RTSP/TCP；本子集没有修改解码、媒体发送或观看授权路径。窗口沿用传统监控桌面外观，“基本”页显示本次查询结果，云台与预置位仍为占位。

## 接口与语义

```http
POST /api/devices/{device_id}/query
Content-Type: application/json

{"command":"DeviceInfo"}
```

`command` 仅接受 `DeviceInfo`、`DeviceStatus`。查询对象为已注册设备；在 Qt 中选择通道时，仍查询所属设备，不声称返回通道级状态。

成功的业务响应包含 `device_id`、`command`、`sn`、`result`，及对应的 `info` 或 `status`：

- `info`：`device_name`、`manufacturer`、`model`、`firmware`、`channel`。通道数使用可选整数，区分缺省和明确的 0。
- `status`：`online`、`status`、`device_time`、`encode`、`record`、`reason`。Qt 明确标记“设备自报”，不替代平台注册、Keepalive 所确定的在线事实。

SIP 200 仅代表查询受理，HTTP 成功还必须收到匹配的独立 XML Response。先到 XML、后到 SIP 200 可以成功；先到 XML 但随后 SIP 拒绝仍是失败。设备 `Result=ERROR` 以 HTTP 200 返回业务结果，Qt 明确显示“设备拒绝”，不当作取得有效信息。

入口参数错误为 HTTP 400，不存在的设备为 404，离线或正在停止为 409，SIP 拒绝为 502，内部总期限耗尽为 504。内部查询期限 5 秒，Qt HTTP 期限仍为原有 10 秒；没有自动重试。非法设备 XML 回 SIP 400，不交付查询结果；若没有后续合法响应，HTTP 查询到期为 504，而不是 HTTP 400。

依据 [GB/T 28181—2016](https://ossrs.net/lts/zh-cn/assets/files/GBT28181-2016-3f992e659a46001a1c89c05db539a5d3.pdf) 的设备查询流程及 XML 定义处理，不复制另一套 Catalog 缓存。当前严格接受 `OK/ERROR`，成功 DeviceStatus 要求 `ONLINE/OFFLINE` 和 `OK/ERROR`；没有真实兼容缺陷证据前，不加入大小写转换或兜底结果。

## 状态、所有权与最小实现

signaling 的唯一新增运行状态是短寿命 `pendingQueries`：SN 映射到设备编号、命令和容量为 1 的结果 channel，使用已有 `live.mu` 保护。发送前登记，HTTP 返回时 defer 删除，不关闭 channel；无新增数据库字段、缓存、后台任务或业务状态机。

接收端先校验 From、当前有效注册来源和停止状态，再按 SN、设备编号、命令关联。当前来源合法的未知、迟到、不匹配或重复结果回 SIP 200 后丢弃，不改变其他请求；原 Catalog 对过期 SN 的 SIP 400 行为保留，两者不是同一套聚合语义。共用原子 `messageSN`、三字段 `manscdpQuery` 和实际 MESSAGE 发送函数 `sendMANSCDP()`，不保存旧 endpoint 快照。

Qt 新增一个从属当前列表的 QObject 查询 context，只在发请求时创建。切换设备、A→B→A、刷新、换平台、关闭时销毁旧 context，旧回复不能覆盖当前目标、按钮或结果；回调只捕获设备编号和命令值，不捕获树节点。网络 reply 仍由原 `platform_client` 在完成/有界超时后回收，context 失效不代表立即 abort 网络。结果使用只读纯文本，不缓存、不自动轮询。没有新增 started/closed、重复连接状态或 token 状态。

## RED / GREEN 与最终测试

所有路径均在 DEV，原始证据可能含本地测试连接信息，不提交原始日志。下表的查询证据根目录为 `/var/tmp/media-server-qt-stage3-query-RjkDSe9c/`。

| 检查 | 实际结果 | 证据 |
| --- | --- | --- |
| Go 新入口 RED | 原入口 404，测试要求参数错误 400 | `/var/tmp/media-server-qt-stage3-query-RjkDSe9c/endpoint-red.log` |
| Qt 查询 RED | 原 14 项通过，新 11 行因缺少控件失败 | `/var/tmp/media-server-qt-stage3-window-tests-I1Pb7bPn/red-qtest.log` |
| Go 最终查询定向 | 7 个新增顶层测试通过，11.540 秒 | 查询证据根目录 `query-final.log` |
| Go 全量 normal | PASS，signaling 30.775 秒 | 查询证据根目录 `go-test-controlled-final.log` |
| Go 全量 race | PASS，signaling 30.152 秒 | 查询证据根目录 `go-race-controlled-final.log` |
| Go vet | PASS，空输出 | 查询证据根目录 `go-vet-final.log` |
| 桌面 normal | 4/4 CTest，77 项 QTest，无失败/跳过，55.98 秒 | `/var/tmp/media-server-qt-stage3-main-Y4xy1xyZ/final-normal-ctest.log` 及 `normal/Testing/Temporary/LastTest.log` |
| 桌面 ASan+UBSan | 4/4 CTest，77 项 QTest，无失败/跳过，56.04 秒 | 同目录 `final-asan-ctest.log` 及 `asan/Testing/Temporary/LastTest.log` |

Go 使用 Go 1.27.1、`GOMAXPROCS=2`、`-p 1`。normal 使用 `CGO_ENABLED=0`，race 使用 `CGO_ENABLED=1`。桌面使用 GCC 16.0.1 experimental、Qt 6.2.4、私有 FFmpeg 9 shared SDK；normal 为 `-O2 -g -DNDEBUG -Werror`，ASan+UBSan 为 `-O1 -g -DNDEBUG -fsanitize=address,undefined -fno-omit-frame-pointer`。没有关闭 leak detection 或屏蔽 sanitizer 报告。Qt/SDL/FFmpeg 等外部库不是完整插桩范围。

Go 最终期限测试增加“至少等待 4 秒”的下界断言后，normal 定向和全量已覆盖；另以最终文件单独执行 `go test -p 1 -race -count=1 -run TestDeviceQueryTotalDeadline -v .` 通过，测试 5.23 秒，包耗时 6.242 秒。证据为查询目录 `query-deadline-final-race.log`。该补充没有修改生产期限。

桌面四组 QTest 分别为 12、11、24、30 项（包含各组初始化/清理）。新窗口覆盖 DeviceInfo/Status、非 GB 源不可查询、不合法 HTTP/业务结果、设备拒绝、真实 10 秒客户端超时、通道对应父设备、迟到响应、A→B→A、刷新、换平台、正常关闭及不经过 closeEvent 的直接析构。最后一项 normal/ASan 均通过，未复现理论上的析构期访问问题，因此没有为此新增 disconnect、防御状态或特殊析构层。原播放、布局及关闭用例保留。

新增网络超时用例使窗口测试实耗约 18.6 秒，因此仅将 `desktop_monitor_window` 的 CTest 外层预算从 15 秒改为 30 秒；不是放宽生产网络期限或媒体健康断言。

Go 定向覆盖真实 UDP Query/Response、XML 早到、仅 SIP 200 无 XML、先 XML 后拒绝、取消、错误 SN/命令/设备/From/来源、重注册、停止、重复/迟到、GB2312、缺省/零 Channel、并发及生产 5 秒期限。详细命令及结果见 `query-review.md`。

### 必须保留的失败记录

首轮完整 Go 测试 `go-test-full.log` 曾 FAIL：未修改的 `TestPushCredentialConcurrentConsumptionAcrossConnections` 报 `SQLITE_BUSY`。随后在独立导出的原基线 `7ea9666` 对同一测试重复 30 次，28 PASS / 2 FAIL，同一错误，证据为 `baseline-credential-repeat.log`。没有修改数据库、原断言或凭据生命周期；最终受控完整重跑通过不抹去该既有波动。

旧 Catalog UDP 测试仍可见既有 sipgo 清理警告；未修改第三方或屏蔽日志。它不构成“整个项目从无警告”的结论。

## 实际应用组合验收

`/var/tmp/media-server-qt-stage3-query-RjkDSe9c/gui-run-01/` 保留首轮结果及 4 张截图，旁边 `gui-run-01.log` 为私有组合测试日志。实际链路是：

```text
gb_monitor ELF / 私有 Xvfb 真实按钮点击
  → Go HTTP handler
  → UDP SIP MESSAGE Query / Response 受控 peer
  → Qt 纯文本结果显示
```

两条请求为 DeviceInfo SN 1、DeviceStatus SN 2；真实界面分别显示测试信息和 OFFLINE 状态，原注册 online 不变，最后 pending 为 0。5 项检查全 PASS，应用和 Xvfb 正常退出并回收；Go 组合入口 PASS 0.843 秒。不是 mock HTTP，但设备注册由测试前置，未执行摄像机 REGISTER，也未操作物理设备或播放音视频。

- `gb_monitor` SHA256：`20e815817c19dbd0d3abe38e4e7c2cfeab913a761513a7d08ce058a558e9d113`。
- `gui-run-01/result.json` SHA256：`d520b7911db0da12b64f7a3707ce887094240f7a1cf1635ed45532544d0ca5f2`。
- `gui-run-01.log` SHA256：`6998783f4862e33fcdc61d4ffa2cf76e55723bca6eb07dfe4e7945fa5f5a6aa3`。

私有组合脚本与源码副本仅保留在证据目录，未为一次编排新建生产接口或通用测试框架。

## 复审与后续边界

Claude Code 的设计、实现及最终只读复审已完成，原文按证据文件名 `round-15.json`、`round-16.json`、`round-17.json` 保留在 `/tmp/qt-client-plan-claude-JuZHjy/`（以文件名追溯，不依赖回复正文自称轮次）。实现复审提出的空闲查询 context 分配已删除，直接析构疑点已用 normal/ASan 用例实际检查。最终复审未发现可达 P0/P1 或提交阻断，认可严格枚举、SIP 拒绝不兜底成功和硬期限选择，允许只提交、推送开发分支。根代理最终检查 diff、格式与证据后提交。

PTZ、预置位、录像回放、下载、告警及对讲仍非本子集范围。PTZ 的 SIP 200 仅表示控制受理，不能证明物理动作或 STOP；现有模拟器统一 MESSAGE 200 也不构成动作证据。取消 sipgo 事务不发送物理停止命令，后续控制设计必须单独覆盖停止顺序和真实硬件。只读调查保留在 `/var/tmp/media-server-qt-stage3-query-RjkDSe9c/ptz-transaction-readonly.md`。

本次不继承阶段二 16 路性能结论，不重新跑容量、不宣称第二机或公共发行许可通过。查询子集可单独交付，整个阶段三及七阶段 Goal 尚未完成。
