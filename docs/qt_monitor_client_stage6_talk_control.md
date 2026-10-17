# 阶段六：对讲控制与媒体上传闭环

## 范围与所有权

本增量接续内部 Broadcast SIP owner 和[一次性音频上传](qt_monitor_client_stage6_audio_upload.md)，不增加独立广播产品。Go 只控制设备 SIP 与可信媒体接口，不接收或转发 PCM/PCMA，不增加数据库字段、第二个会话表、消费状态或关闭标志。

每次启动独立随机 64 位小写十六进制媒体 ID，区别于持久高水位的 20 位 SIP SourceID；上传 token 另行生成，只在成功响应中交付。媒体 ID 不复用，旧 talk_id 删除与迟到对账均不能关闭新 owner。

启动使用一个整体 inviteTimeout（默认 10 秒）：媒体 create 实际绑定端口 → 原 Notify/独立业务 Response/反向 INVITE → 双 ACK 路由与门控 → 媒体 configure 实际完成 → HTTP 201。端点、PT8/PCMA、SSRC 来自既有严格 SIP 校验，不接受调用方指定设备媒体目的地址。

HTTP 只拥有启动阶段。失败或启动取消显式取消 owner 并等待其 done；成功交付后不因原 HTTP context 结束而杀掉对讲。独立 owner 仍由设备下线、删除、远端 BYE、平台 shutdown、媒体租约失败和显式停止管理。

## 公共 API

| 请求 | 行为 |
| --- | --- |
| `POST /api/devices/{device_id}/talk`，JSON `{}` | 成功 201，返回 `talk_id` 与 `upload_url`，`Cache-Control: no-store` |
| `GET /api/devices/{device_id}/talk` | 返回当前未取消控制 owner 的 ID；无 owner 为 404；不返 token，不代表设备收到声音 |
| `DELETE /api/devices/{device_id}/talk`，JSON `{"talk_id":"…"}` | 仅关闭匹配 ID 的本次 owner，清理完成 204，不匹配 404，清理错误 502 |

无效输入 400，设备不存在 404，离线/停止围栏/单设备冲突 409，媒体或 SIP 启动失败 502。响应写入失败使用独立有限清理 context 关闭同一 ID，不按设备盲删。

上传 URL 当前是媒体 IPv4/HTTP 端口的 `ws://…/publish/gb28181/audio/<id>/<token>`，直连 LAN 子集。音频消息必须每条 160 字节 PCMA/8000/mono（20ms）。既有 C++ owner 原子认领并永久消费 token；Go 不保存另一份消费状态。此版本未实现网关 WSS 地址发布与浏览器安全上下文适配，不据此宣称 Web 麦克风就绪。

## 失败与回收

- create 明确 HTTP 拒绝时未取得媒体所有权，不删除已有 ID；响应未知或内容无效时以本次唯一 ID 补偿删除，404 视作当前已无。
- 补偿和正常结束先删除媒体会话，再终止/等待 SIP answer 与 BYE，保留各层清理错误；远端 BYE 不额外发 BYE。
- 模糊 create 的迟到创建以及控制删除无法到达时，仍依靠 C++ 已有 20 秒控制/音频期限兜底，不能声称删除响应代表 socket 已同步释放。
- 音频不混入视频 receiver 清单。原对账入口先给已确认绑定端口的音频 owner 发送 status 续租；任一续租失败取消同一 owner，不重试或自动重连，不将网络未知说成设备离线。旧快照使用对象 identity 围栏。
- 平台停止沿原 broadcast cancel/done；每个父子对象仍按既有 shutdown/post/safe_shutdown 清理。没有新后台任务，复用原对账周期。

## 验证与边界

私有证据：DEV `/var/tmp/qt-talk-control-5c6ec00-RvucT9Uh/`。原始 URL/token、SIP 与网络日志不提交。

已建立公开 API 缺失的 RED（`api-before.log`）。新增失败覆盖 create 明确拒绝、不合法端口、响应未知、configure 拒绝/取消、响应写入丢失；补齐请求寿命交接、旧 ID、续租失败与替代 owner 围栏。

`directed-race-final.log` 保留测试夹具自身的 90 秒超时：无网络读写的假 HTTP handler 等 request.Context，并不保证能观察客户端超时。只将夹具改成已有 cleanup 通道释放，使迟到 201 真实发生并有界回收；不改生产超时或放宽断言。修正后的 `directed-race-corrected.log` PASS（9.586 秒）。

实际 Go→生产 C++→真实 WebSocket→UDP peer 三组：显式 DELETE、上传连接关闭、设备 BYE。Normal 3/3（15.668 秒），ASan+UBSan 3/3（16.168 秒）；每组验证 4 个精确 PCMA RTP 包、序号/时间戳/SSRC/payload、真实 RTCP SR 计数、token 重放 403、SIP BYE/200、owner join、媒体 status 404 与公开 GET 404。两个服务进程正常退出；ASan 开启 LeakSanitizer，保留 Boost makecontext/swapcontext 既有警告。

普通 wire 测试的媒体控制使用 httptest 假服务，SIP 走真实 UDP；只有上述显式集成用生产 C++。`TestTalkRealMediaUpload` 默认 SKIP，必须提供其受控 loopback 媒体环境才执行，不能把普通 Go 全量测试当作这三项真实链路执行。

最终 Go `test -count=1 ./...` PASS（主模块 126.043 秒，simulator 0.002 秒），`vet ./...` PASS，`test -race -count=1 ./...` PASS（主模块 132.471 秒，simulator 1.012 秒）。日志分别为 `go-test-final.log`、`go-vet-final.log`、`go-race-final.log`。独立只读复审覆盖 owner、媒体补偿与既有设备下线/删除/平台停止调用链，未发现可达 P0/P1；未新增生产状态或数据库迁移。

本项没有 Qt/Web 麦克风、真实摄像机端听音或设备回传音频验收。201 是协商/配置成功，不是媒体发送成功或双向声音成功；整个阶段六仍未 GO。
