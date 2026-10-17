# 阶段六增量：一次性音频上传与媒体会话

## 范围

基于 `dc208c7`，在 `feature/qt-monitor-client` 增加 Qt/Web 可共同使用的私有 WebSocket 音频入口。音频由客户端直接发送 C++，不经过 Go；此增量仍不是完整对讲，尚无公开 Go 对讲 API、Qt/Web 麦克风采集或真实设备双向音频验收。

复用前一阶段的 `gb28181_audio_sender`、现有端口池和 `session_registry` sender 条目。音频没有视频媒体源，不出现在 `/receivers` 中，不改变原视频对账。当前仅支持已明确的 IPv4 直连、UDP、PCMA/8000/mono/PT8。

## 控制接口

四个接口均为 `POST application/json`，沿用媒体控制面 Bearer/未配置时仅 loopback 的授权规则，不将控制凭据交给客户端。会话 ID 和上传 token 分别生成独立的 64 位小写十六进制随机值，不使用设备、通道、SIP SourceID 或既有视频源 ID。

| 路径 | 请求 | 成功语义 |
| --- | --- | --- |
| `/gb28181/audio/create` | `session_id` | 201，返回已绑定的 `rtp_port`；RTCP 为其相邻端口；尚不能上传 |
| `/gb28181/audio/configure` | `session_id, upload_token, remote_address, rtp_port, rtcp_port, ssrc` | 204，实际在 owner 上完成端点和 packetizer/RTCP 配置，不只是投递 |
| `/gb28181/audio/status` | `session_id` | 204，当前会话存在并请求续控制租约；不是音频收发健康证明 |
| `/gb28181/audio/delete` | `session_id` | 204，撤销 registry identity 并请求关闭；不是等待所有网络资源释放 |

不存在返回 404；重复 create/configure 返回 409，不旋转旧票据；非法字段、地址、端口、SSRC 返回 400；方法与 Content-Type 错误分别为 405/415。配置失败立即撤销 registry 并关闭，不能重试同一对象。201 响应写出失败会补偿关闭；响应写成功但调用方未收到的情况还须 Go 以相同 ID 删除，不能假定 create 未执行。

SIP 调用方必须先 create，使用真实绑定端口协商，取得设备端点及 SSRC 后 configure 成功，最后才能交付上传 URL。HTTP 创建请求的取消不能直接作为完整对讲寿命；Go 后续集成需持有独立 owner context，监听 Broadcast result ctx，并在媒体端明确消失或不能续租时结束 SIP。客户端不能通过上传参数指定设备 UDP 目标。

## 上传契约

```text
GET /publish/gb28181/audio/<session_id>/<upload_token>
Upgrade: websocket
每个 binary message = 160 字节 PCMA = 20ms
```

HTTP/1.1、无 query、无 body、无读取后残留字节；消息可按 WebSocket 正常分片后聚合，但完整消息上限 160 字节。文本及非 160 字节消息结束会话。客户端负责实际采集节奏和有界发送，不得积压旧麦克风声音或无限加速重放。此入口不承诺服务端重采样、自动重连或音频可靠重传。

owner 在无挂起窗口中验证 token 和 registry identity、永久消费 token、取得唯一 WebSocket。错误 token 不消费；认领后的握手失败不恢复。并发同 token 只能一个 101。跨 worker 先 release native socket，再 assign 给实际 owner 的 io，不直接把原 executor 的 socket 移过去。

两个现有 `idle_timer` 表达不同事实：

- 控制租约：create 开始，configure/status 续期；超过 20 秒没有有效控制则关闭。持续上传不能代替 Go/SIP owner 存活。
- 音频 idle：create 开始，成功 configure 后重新给足 20 秒，之后仅合法音频续期。持续 status 不能让未连接或没有有效音频的会话永久存在。

Beast 另有 5 秒握手期限，不再叠加第三个音频 idle。状态查询只表示当前已登记，关闭交错时可能最后返回一次 204；Go 必须按同一会话 identity 处理后续结果，不能把旧结果用于新会话。

## 生命周期与复杂性

`gb28181_audio_session` 是上传 owner，持有 WebSocket 和音频 sender 子对象；子对象错误回调强引用父对象，父 safe_shutdown 调用子 shutdown 并 reset，子 safe_shutdown 清 callback 后解除环。父/子均由自身 shutdown post 清理。关闭 socket 后保留 WebSocket 对象，直到 accept/read completion 退出，避免在途操作访问已销毁 stream。

新增的 `upload_token_` 是未消费授权事实，`upload_` 是已认领网络资源；没有另存 ready/started/closed/consumed 标记。registry 只增加对称的 `find_sender_session`，不新增 map、锁、token registry、数据库或配置。

configure 在 owner 执行后 post 结果回 HTTP caller；临时 owner work guard 和 stop 检查保证正常排空。caller 的存活由锁定 Boost 1.92 `spawn_entry_point` 的 `handler_work` 保证（`boost/asio/impl/spawn.hpp` 的构造、成员及完成路径），不重复加额外 caller guard 或同步等待。

## 验证与证据

DEV 独立证据：`/var/tmp/qt-audio-upload-dc208c7-ONvzPjtA/`。Normal/ASan 沿用上个子阶段明确保留的独立 build 目录，未修改生产 Boost 安装。

新 `gb_audio_upload` 六组真实 HTTP/WebSocket/UDP 测试覆盖控制授权、严格身份、跨 worker 配置与立即上传、PCMA payload/SSRC/sequence/timestamp、并发唯一认领、非法消息不产生 RTP、主动删除、网络断开、worker stop、重复关闭、weak 最终释放及四对端口重新绑定。控制租约组持续发送并接收真实 RTP，在第 10 秒取得一次 status 204，要求至少运行 29 秒、至少收发 100 包、末 RTP 在第 28 秒后，再因没有后续控制续租关闭；这排除了 keep_alive 为 no-op 仍通过的情况。另一组持续控制但不上传音频，要求真实音频 idle 在约 20 秒关闭。

测试复审发现初版 WS close 后立即执行 fixture 停机可能掩盖自动注销，已改为未主动 shutdown/stop 前观察 status 404；控制租约初版只计客户端写成功，现并行验证真实设备端 UDP 收包。未放宽时间、帧或资源断言。

首轮编译缺少直接 TCP/core include，以及新增 CMake target 尚未重新 configure 的失败日志均保留；它们不算生产行为 RED。首轮六组定向通过 40.07 秒，最终结果以本目录后续完整回归及 sanitizer 原始日志为准。

完整 RelWithDebInfo 构建通过，CTest 71/71 通过（511.38 秒，`ctest-full.log`）。这次全套使用最终生产代码及先前六组测试；其后仅增强上述第 10 秒续租阳性断言，最终增强版须另以定向回归日志核对，不能把先前 40 秒结果冒充增强后的执行结果。ASan 使用既有独立 Boost 1.92 ucontext 前缀，项目兼容守卫真实通过；LeakSanitizer 保持开启。第三方 FFmpeg/OpenSSL 等外部库不因此视为全部插桩。

增强后 Normal 定向回归六组通过，CTest 1/1（50.06 秒，`normal-renewal-final.log`）；同一组测试没有真实麦克风或相机参与，只证明真实 HTTP/WebSocket/UDP 环回链路。

最终 ASan+UBSan 定向 CTest 4/4 通过（66.40 秒，`asan-directed-final.log`），包括 `gb_audio_upload` 六组增强测试、`gb_audio_sender`、`gb_sender_lifecycle` 和 `udp_transport`。启用 `detect_leaks=1:halt_on_error=1` 与 UBSan `halt_on_error=1:print_stacktrace=1`；没有 ASan/UBSan/LeakSanitizer 错误，保留 Boost.Context 既有 makecontext/swapcontext 支持边界提示。本轮未执行完整 ASan CTest，不能由这四项扩展为全部第三方或真实设备内存安全结论。

DEV Claude Code 两轮生产代码只读复审未发现 P0/P1，意见及修正保存在 `claude-review-01.json`、`claude-review-final.json`。最终续租测试追加复审返回周额度耗尽（`claude-renewal-review.json`），不是审查 PASS。2026-10-17 用户明确改为由当前开发代理自行开发、审查和验证，不再以 Claude 复审为提交门槛；随后独立只读代码审查无 P0/P1，确认增强测试能排除续租 no-op。可信调用方每次必须生成全新媒体 ID，不支持同 ID 删除重建；创建响应失败补偿不扩展为支持同 ID 复用的代际协议。Go 对讲闭环目前只有只读设计：沿用唯一 broadcast owner/map，以独立媒体 ID 区分 SIP SourceID；启动请求取消显式取消并 join，成功后由既有 owner/shutdown 接管，不额外增加 AfterFunc 桥、关闭状态或第二注册表。

## 后续边界

尚须 Go 公共入口和 SIP owner 双向结束闭环、Qt/Web 采集及编码、有界发送、Web 安全上下文/WSS、真实设备扬声器与返回音频验收。本增量只证明受控网络输入的内容和生命周期，不替代麦克风、声学、设备双向音频或完整阶段六 GO。设备录像 Scale 和同会话 seek 亦未在本增量实现。
