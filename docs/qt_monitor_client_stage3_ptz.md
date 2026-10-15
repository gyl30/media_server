# Qt 阶段三：有界云台点动与停止

## 范围与协议边界

基线 `98a5f8baad06031b933d4d0f329c1b69703e74fe`，开发分支 `feature/qt-monitor-client`。本子集实现八方向点动、变倍和 STOP，不实现聚焦、光圈、巡航、预置位写操作、录像或对讲；整个七阶段工作尚未完成。媒体播放、票据、数据库和共享上游不变。

指令依据 [GB/T 28181—2016](https://ossrs.net/lts/zh-cn/assets/files/GBT28181-2016-3f992e659a46001a1c89c05db539a5d3.pdf) §9.3、附录 A.3.1–A.3.2、J.6，使用 DeviceControl XML 与八字节 PTZCmd；父注册设备承担 SIP 路由，XML DeviceID 指向明确通道，校验和是前七字节之和的低八位。协议允许的控制地址字段不用于选择通道，不增加另一份设备地址配置。ControlPriority 使用示例值 5，不代表平台实现了设备端抢占/权限策略。

```http
POST /api/devices/{device_id}/channels/{channel_id}/ptz
Content-Type: application/json

{"command":"left","speed":64}
```

命令为 `left`、`right`、`up`、`down`、`up_left`、`up_right`、`down_left`、`down_right`、`zoom_in`、`zoom_out`、`stop`。方向速度为 1–255，变倍为 1–15；STOP 不带 speed（也接受 0）。不接受任意时长。参数校验失败不发送 SIP。没有既有 owner 的新请求还须满足父设备存在、非停止中、在线、目标属于当前 Catalog；检查在线早于 Catalog，避免离线清理后误报 404。已有 owner 的 STOP 直接请求其取消 START/等待，不因注销后注册记录已移除而被新的在线检查挡住。

普通点动以首次发送前的绝对期限为准，最多等待 300 毫秒；SIP START 的 Do 返回、本地事务重传终止后，再尝试 STOP。START 被拒绝、超时、HTTP 断开或手动停止，均不能省略该 STOP。STOP 使用独立后台 context，最多 1.5 秒，不继承请求或动作取消信号。它仍发往本次动作持有的原注册快照，不在注销后重新获取路由，不猜测新 endpoint。

这里限制的是平台等待和停止尝试，不是物理运动时长保证。UDP 丢失、设备失联、网络乱序、进程崩溃或 kill -9 时不能承诺摄像机确实停止；SIP 200 也只表示协议受理。STOP 失败必须明确返回并记录，不自动无限重试。

## 响应与运行所有权

成功点动为 HTTP 200，含父设备、通道、原 command、`start: accepted`、`stop: accepted`、`physical_result: unconfirmed`。无正在执行的动作时，独立 STOP 同样返回 HTTP 200，但不需要 start 字段。START 事务失败后即使 STOP 成功仍返回 502 `ptz_start_failed`；它表示 SIP 事务未成功完成，绝不证明设备没有动作：200 丢失、迟到或本地取消时，设备仍可能已执行。STOP 失败优先返回 502 `ptz_stop_unconfirmed`，不能降格为正常成功。

已有同通道 owner 时，普通点动返回 409；手动 STOP 仅取消该 owner 的 START/等待阶段并返回 HTTP 202，含 `command: stop`、`stop: requested`、`physical_result: unconfirmed`。202 不是 STOP 完成证明。第二次 STOP、设备注销或停机不能取消已在进行的独立 STOP。不同通道可并行。

唯一新增运行事实是 `liveService.ptzOwners`，保存 `liveKey → CancelFunc`，复用现有 `live.mu`；条目由原 owner 在 STOP 尝试结束后删除，不由后来的 STOP 请求删除。没有新持久表、结果缓存、done channel、额外锁或关闭标志。设备注销、过期、删除经既有 beginDeviceStopLocked 取消对应点动，HTTP 停机在 drain 前取消当前 owner 快照；不声称快照能取消尚未登记的请求。

main.run 为 SIP 使用保留 context value 的独立取消 context。正常停机时 HTTP 先执行既有五秒 Shutdown/drain，返回后才停止 SIP，保证 STOP 尝试期间监听仍能收应答；任一监听启动失败仍有界退出。不新增停机协调器或改变既有 live/BYE 清理。该五秒边界不承诺任意调度冻结或内核写阻塞下都能排空。

## Qt 界面和取消

目标显示移到三个控制页的公共位置，通道同时显示父 ID 与通道 ID；基本页仍查询父设备，云台只控制明确通道。方向和变倍速度为两个实际输入控件，默认 64/4，范围各自明确；按钮没有自动重复、按下保持或 Qt 点动定时器。

复用 `control_context_` 隔离查询/动作结果，普通请求相互禁用，STOP 在当前为国标通道时始终可用。STOP 接管时清除旧查询等待提示；A→B→A、刷新、换平台、关窗和直接析构均隔离旧回调。context 销毁只取消 UI 等待，不撤销已发 HTTP/SIP，也不能作为物理停止保证。STOP 接管后，旧点动最终 STOP 的失败只会留在平台日志中，不再更新已被替换的 UI 结果。

新增成员均为实际控件引用，不增加 busy、第二份目标、动作缓存或生命周期状态。`set_control_busy(bool)` 供多个请求入口和完成路径共用，启用范围直接由当前树节点和参数派生，不保存另一份忙碌事实。

## 验证记录

下列结果均针对本子集代码及同一 shared decoder SDK，不继承阶段二的 16 路或物理设备结果。

| 验证 | 实际结果及证据 |
| --- | --- |
| 新路由 RED | `endpoint-red.log`：旧路由返回404，期望400 |
| 停机时序 RED | `run-drain-red-02.log`：旧共享 context 在 HTTP drain/XML屏障期间关闭 SIP，Keepalive 超时，查询最终504 |
| Go 定向 GREEN | `ptz-directed-02.log`：9个顶层测试，PASS，7.926秒 |
| Go 全量 test / race / vet | 两包均PASS；signaling 42.274 / 47.872秒，simulator 0.002 / 1.008秒；vet退出0 |
| Qt 定向 RED/GREEN | `red-qtest.log` 缺少STOP控件，2 PASS / 1 FAIL；新增44项 GREEN，8.753秒；最终窗口完整103项，35.037秒，无失败/跳过 |
| 最终 normal 桌面 | `normal-ctest.log`：4/4 CTest，73.32秒；150项QTest（含初始化/清理），无失败/跳过 |
| 最终 ASan+UBSan 桌面 | `asan-ctest.log`：4/4 CTest，75.39秒；150项QTest，无失败/跳过；实际加载两个runtime，无关闭leak detection或suppression |
| 实际 Qt → HTTP → SIP | `gui-run-01.log` PASS，1.913秒；实际发行ELF/XTest点击left64，受控UDP peer各收到一次 START/STOP，owners/pending归零 |

正常及 sanitizer 全量各为 platform 12、decoder 11、playback 24、window 103项。窗口既有59项保留，新增44项；仅外层该CTest预算由30秒调为45秒，以容纳已测35秒的完整用例，生产10秒请求期限及断言不变。独立桌面不依赖Boost.Context，未绕过服务器的兼容守卫；系统Qt/SDL/FFmpeg未完整插桩，本结果不扩大为所有第三方库ASan验收。

Go覆盖START拒绝/应答丢失后仍STOP、无后续本地START重传、STOP拒绝/超时、连续STOP、实际HTTP取消、设备注销/过期/删除、不同通道并发和owner归零。真实`run()`测试包括Digest REGISTER、Catalog和被屏障阻塞的DeviceInfo：停机期间约150毫秒内15次独立Keepalive受理，PTZ STOP完成后才释放XML，查询返回200，最后run退出；HTTP和SIP绑定失败也有界退出。首个`run-drain-red.log`是私有peer readiness缺陷导致bind冲突，不是生产RED；初版readiness额外UA的清理warning及旧高频探测日志保留，最终使用raw UDP OPTIONS且有限10毫秒探测，不隐藏工具失败。取消与迟到START200竞争的第三方`Unhandled sip response` INFO保留，不解释为物理结果。

Qt覆盖所有动作参数、19类无效响应、8行STOP接管、14类ABA/刷新/平台切换/退出隔离。最终组合使用实际GUI，不是mock HTTP：私有Catalog/注册fixture、真实Go handler与UDP MESSAGE；没有将本组合说成真实摄像机注册验收。`04-real-ptz-results.png`已人工检查，最终显示“点动与 STOP 均已受理，物理结果未确认”。截图SHA256 `0b231dc2b260cedf6d480dcf6f32a6e9013232913b5001cc3599f7da08ab44d3`；result SHA256 `fc46341d4fea04f5b64455c82c6336a31c5aaea7a1c795b819137ab739773776`。应用与Xvfb正常退出0、reap并确认PID816504/816501均不存在。

本次实际GUI ELF SHA256：`36b5745a72f1a111ca8fcd057e4b004ce38f91686c58a7cab4e5b115d5a892a4`；ASan ELF：`c4619863500b0cfd55ab38368ae8dc39d77667a64c4698d5f810fb94242ee814`。完整构建SHA清单见`build.sha256`，Go冻结输入见`source-sha256.txt`，Qt测试输入SHA256 `843d91b00446cb4fced35b8faddd4bec9a2a57b4ea3a5cc33022fec8c6b1489c`。

- Go 原始证据：DEV `/var/tmp/media-server-qt-stage3-ptz-TcYTJA2n/`。
- 桌面最终构建和组合脚本：DEV `/var/tmp/media-server-qt-stage3-ptz-main-8CZY6OxW/`。
- Qt 定向 RED/GREEN：DEV `/var/tmp/media-server-qt-stage3-ptz-tests-SERSNLBH/`。
- Claude 设计/实现/最终讨论：DEV `/tmp/qt-client-plan-claude-JuZHjy/round-21.json`、`round-22.json`、`round-23.json`；最终核对源码、原始结果及边界后未发现P0/P1，同意本子集独立提交及推送开发分支，不合main。

复杂性复审保留已有owner优先于数据库查询的顺序：已有owner的STOP仅请求取消，不先做DB读取；若把读取移出锁又保持这一路径，需要拆分及再次加锁/复核，增加分支。删除/注销本身已经取消对应owner，不能声称单纯前移DB读取就一定丢失停止请求。数据库读取沿用既有live锁范围，不新增异步准备状态。STOP使用经标准及测试核对的固定字节，不另建编码缓存或为了消除字面量增加忽略错误的调用。接口错误指事务结果，物理动作始终未确认；以上不是额外物理保证。

阶段清理仅删除已核对realpath、CMakeCache来源且无运行构建的重复定向构建中间目录：`/var/tmp/media-server-qt-stage3-ptz-tests-SERSNLBH/red-build/CMakeFiles`（23MiB）、`normal/CMakeFiles`（26MiB）。可从保留源码重新构建；原始RED/GREEN日志、Testing、ELF、私有fixture和失败运行保留，最终主normal/ASan构建继续保留。本子集没有启动或清理TEST/摄像机进程，不修改主工作区、正常SDK、原型或第三方。项目公共分发许可仍未闭合，内部构建通过不代表公开发布授权。

真实可动设备尚待确认，物理云台、变倍及停止验收为 BLOCKED。受控 UDP peer、HTTP 成功与界面提示不能代替物理动作证据。不访问历史摄像机，也不继承此前厂商验证 PASS。
