# GB28181 设备管理与一次性 WHEP 点播 MVP

验证日期：2026-10-03。

## 基线与提交

- BASE_HEAD：`f0c61cd5d6137234ea4b604b48f60ebb2cc3c20a`。
- CODE_HEAD：`1740fec5f20172c7cd098150e596e934ca2722be`，全部生产修改的最终提交。
- FINAL_HEAD：包含本文的文档提交。完整 SHA 可通过 `git log -1 --format=%H -- docs/gb28181_signaling_mvp.md` 取得，并在最终交付报告中记录。本文不嵌入自己的提交 SHA。
- 开始时 worktree clean，HEAD == origin/main。以下阶段均在验证后 commit/push。

| 提交 | 内容 |
| --- | --- |
| `102b373` | 接入国标设备管理与注册授权 |
| `be4808a` | 接入国标通道复用与一次性 WHEP 点播 |
| `e1aa982` | 收口国标设备删除与离线清理 |
| `d13e984` | 补充国标设备点播端到端验证 |
| `1740fec` | 移除国标旧控制入口 |

只修改 signaling、simulator、测试和验证文档；`media/`、`service.*`、CMake 生产目标没有变化。

## 业务对象和职责

| 对象 | 所有者与存储 | 语义 |
| --- | --- | --- |
| device | signaling，SQLite `gb_devices` | 仅持久化不可修改的 20 位数字 `device_id` 与非空 `name` |
| registration | signaling `deviceRegistry`，内存 | SIP Contact、endpoint、注册有效期、Keepalive 和 online；不决定持久设备是否存在 |
| channel | signaling `channelRegistry`，内存 | 当前 Catalog 通道；offline 清空，重新上线再同步 |
| live_session | signaling `liveService`，内存 | 一次 device/channel UDP 收流 generation；拥有 SIP dialog、receiver identity 和 SSRC |
| play_ticket | signaling `liveService`，内存 | 一个客户端的一次 WHEP offer 尝试；不代表上游 live 或已建立 viewer |

`device != registration`、`offline != deleted`、`Expires:0 != DELETE device`。

signaling 管理设备、SIP 和播放授权；media_server 收 RTP/PS、维护媒体源与 viewer；simulator 只模拟外部设备。

媒体路径为 `device -> RTP/PS -> media_server -> SRTP -> browser`。signaling 只代理第一次 HTTP SDP exchange，没有代理媒体，也没有另建 viewer registry。

## REST API

| 接口 | 成功 | 主要失败 |
| --- | --- | --- |
| `POST /api/devices`，`{device_id,name}` | 201，含 id/name/online | 400 invalid_request，409 device_exists |
| `GET /api/devices` | 200，SQLite 设备列表加 runtime online | 离线设备继续存在 |
| `GET /api/devices/{id}` | 200，id/name/online，可有 runtime last_seen | 404 device_not_found |
| `GET /api/devices/{id}/channels` | 200，当前 channels，可有 live_id/state/stream_name | 404 device_not_found；离线设备 channels=[] |
| `POST /api/devices/{id}/channels/{channel}/play` | 201，live_id/play_id/expires_at/whep_url | 404 device_not_found；409 device_offline；404 channel_not_found；409 channel_offline |
| `POST /play/whep/{play_id}`，application/sdp | 媒体端 WHEP 状态、SDP 与 headers | 404 play_not_found；415 invalid_content_type；400 invalid_offer；502 proxy/network failure |
| `OPTIONS /play/whep/{play_id}` | 200，CORS/POST capability | 不消费 ticket |
| `DELETE /api/lives/{live_id}` | 204 | 404 live_not_found；502 cleanup failure |
| `DELETE /api/devices/{id}` | 204 | 404 device_not_found；409 device_stopping；502 无法确认 receiver 清理 |

设备删除/离线清理期间，同设备的新 play 被拒绝。live 正在终止或 cleanup_pending 时也不能创建下一代。产品 play 响应不暴露 SSRC、RTP port 或 SIP dialog。

RTSP source 的原有 CRUD/start/stop 和 `POST /api/preview/start {source_id}` 保留。

## REGISTER、Catalog 与 offline

REGISTER 先检查持久设备 allowlist：未添加或已删除设备返回 403；已添加设备继续现有 Digest authentication，使用统一 `sip-password`。

offline -> online 自动发起 Catalog；在线 REGISTER refresh 不重复发 Catalog。Keepalive 不能使已 offline 的注册自行复活，需要再次 REGISTER。Catalog/Keepalive 检查与删除 fence 共用短临界区。

Expires:0、registration expiry、heartbeat timeout 都执行：runtime offline、失效 pending tickets、停止设备 live、清理 channels。持久 device 不删除。再次 REGISTER/Catalog 后可开始新的 live generation。

signaling 重启只从 SQLite 恢复 device；registration、channel、live、ticket 均为空。正常 shutdown 会清理本进程 live，不恢复旧 generation。

## 单路上游与并发 play

同一个 `(device_id, channel_id)` 只有一个 live map 项。首次请求按已有流程分配 SSRC/UUID、创建 UDP receiver、发送 INVITE、收到 200 SDP、ACK，进入 streaming。

stream_name 固定为 `gb/<device>/<channel>`；现有 stream UUID 同时作为产品 `live_id`，没有第二个 live UUID。

并发请求复用已有 established lifecycle，等待建立后检查同一 session identity。map reservation 与 SSRC 分配在短锁内，media HTTP/SIP、等待 established、BYE 和 receiver 删除全部在锁外。另一设备的 play 不会被正在删除设备的网络操作阻塞。

每个成功 play 都创建独立 ticket。真实 E2E 8 个并发 play 返回同一个 live_id、8 个不同 play_id；三个 Chrome viewer 使用各自票，设备仍只有一个 INVITE/ACK 和一个 active RTP/PS source。

## Ticket 与 WHEP proxy

ticket 使用成熟 UUIDv4，固定 30 秒，包含 play_id、device/channel、live_id、stream_name、expires_at；仅内存 map。现有 HTTP 生命周期任务每秒统一 sweep，没有 per-ticket timer/goroutine、历史 tombstone 或持久化。

take 在同一锁内 lookup、删除、检查有效期与 live generation；同票并发只有一个请求进入 media_server。过期、已消费、不存在及已失效票统一 404。

语义是 one attempt：错误 Content-Type、过大 body、invalid SDP、媒体 4xx/5xx、不连通、source disappeared 都不恢复票。失败后客户端重新 POST channel/play。票过期不停止 live。

proxy 仅 POST 一个 application/sdp offer，body/answer 上限 1 MiB，HTTP client timeout 加读取/写入 deadline。OPTIONS 支持 CORS，不消费票。

成功响应保留 Content-Type、Location、Cache-Control、Access-Control-Allow-Origin、Access-Control-Expose-Headers。Location 转为媒体端绝对 `/play/whep/session/<id>` 地址，后续 GET/DELETE 直接访问 media_server。

in-flight offer 用现有 live 上的 WaitGroup fencing：take 与 stopping 受同一锁保护，stop 阻止新 take，清理等待已消费 offer 返回后才删除 receiver、释放 generation。它只跟踪 HTTP offer，不是 viewer refcount。`TestLiveStopWaitsForConsumedOffer` 验证旧代 HTTP 建连不会越过 receiver 清理后串入下一同名 source。

Web GB Play 使用正式 play API，不能对同一 ticket URL 重试 409。RTSP preview 的原有 not-ready 等待逻辑保留。

## Stop、设备删除与补偿

DELETE live 找到准确 UUID generation，失效其 pending tickets，进入 stopping，按原有 SIP 时序 BYE，确认 receiver 已删除/404 后释放 SSRC、移除 live。旧 live_id 不能删除新 generation。

DELETE device 先在同一短临界区设置 device fence、失效票、快照所属 live；锁外停止全部 live。确认 receiver 不存在后，才删除 SQLite device、runtime registration 和 channels。

BYE timeout/远端失败只记录日志，不阻止已经确认本地清理的删除。receiver 删除失败且无法确认不存在时，保留持久 device 和 cleanup_pending live；后续 play 被 fence，允许再次 DELETE 完成补偿。

已建立 viewer 依靠现有 `receiver shutdown -> media_stream end -> WHEP on_end`，signaling 不另行管理 viewer 生命周期。

本轮修复一个阻塞验收的原有 rollback 问题：media receiver/create 在 TCP dial 阶段被拒绝，原实现仍作为“可能已创建”补偿；媒体不可达导致遗留 cleanup_pending/SSRC。现在区分请求未送达的 dial error、明确 HTTP rejection 与可能已送达的失败；只有后者保留原有补偿/fencing。RED/GREEN 与真实停止 media 后的 play/recovery 均覆盖。

## Simulator 与删除的旧入口

单设备与 fleet 共用现有 SIP/media engine；删除 simulator HTTP controller、重复的 single-device 驱动，以及 control-url/live-count/start-rate 参数。simulator 只 REGISTER、Catalog、Keepalive、回应 INVITE、处理 ACK/BYE、发送 RTP/PS。退出发送 Expires:0；运行时等待控制器发起下一次 live。运行时长参数为 `--duration`。

独立 `bench/gb_signaling_verify.py` 是管理员/播放客户端，负责正式 API；simulator 不 POST device/play、不 DELETE live。

清理前全仓调用搜索确认 simulator/Web 已迁移，直接移除：

- `/internal/live/start`、`/internal/live/stop`。
- GB channel `/start`、`/stop`。
- `preview/start` 的 GB target 分支。
- 对应 handler、仅旧入口使用的 stop method 和请求类型。

没有兼容 wrapper/fallback。旧路径只在“路由应不存在”的回归测试及本记录中出现。

## 针对性测试

Go 共 17 个顶层 Test，使用临时 SQLite、真实 UDP SIP peer 和系统 HTTP 边界；Go race 通过。主要覆盖：

- device CRUD、校验、duplicate、SQLite 重开；离线列表与单设备可见。
- 真实 SIP 未添加 403、错误密码 401、正确认证 200、Expires:0、删除后 REGISTER 403。
- 并发 live/play 只有一次 receiver create/INVITE，每票独立。
- 同票并发只有一次转发；30 秒精确边界、sweep、错误 SDP/4xx/5xx/不可达/错误类型/超大 body 均消费。
- 旧 live_id fencing、stop 失效票、in-flight offer ordering。
- device DELETE 与 play 并发 fence，另一设备仍可 play。
- heartbeat/registration expiry；BYE 失败仍删除；media cleanup 未确认保留配置并可重试。
- 不可达 create 不残留 live/SSRC；旧路由不存在；RTSP source preview/start/stop 保持。
- simulator 拒绝旧 controller 参数。

## 真实系统 E2E

最终执行目录：`/tmp/media_server_gb_mvp-f0c61cd/e2e-final`。机器：当前本机 loopback；Chrome `153.0.8010.52`，Playwright `1.58.0`，FFmpeg `7.1.1`。独立启动 media_server、signaling、设备 simulator 和真实 Chrome。

最终 E2E 在 `d13e984` 工作区加最终 `1740fec` 改动后构建执行，结果 PASS；之后提交没有继续修改生产代码。结构化结果保存于 `docs/verification_results/gb28181_signaling_mvp.json`，原始 commands/result/日志留在上述目录。

| 场景 | 实际结果 |
| --- | --- |
| 添加设备、离线查询、REGISTER、自动 Catalog | PASS |
| 8 个并发 play | 相同 live_id，8 个不同 play_id，PASS |
| 三个真实 WHEP viewer | H264 framesDecoded：52/27/2 -> 123/98/73，三者 DTLS connected，PASS |
| 共享上游 | invite=1、ack=1、live_active=1、RTP packets=465，send_errors/drop=0 |
| 同票并发、再消费 | HTTP 201/404；再次 404，PASS |
| 自带 Web GB 播放模块 | 正式 ticket flow 解码；停止 viewer 后上游仍运行，PASS |
| 等待 31 秒票过期 | 旧票 404、live 不停止、新票可播放；REGISTER refresh>0、Catalog仍为1，PASS |
| DELETE live | pending票失效，已有 media WHEP resources 404，receiver/delete 404，PASS |
| Expires:0 / 重新注册 | offline、channels空、票/live/viewer清理；新 Catalog、新 generation 可播放，PASS |
| signaling restart | 持久 device 留存、runtime全部空；恢复 REGISTER/Catalog，PASS |
| media 不可达 create | 502、不残留 live，恢复后可 play，PASS；SSRC 无残留由定向 Go 测试证明 |
| WHEP proxy 不可达 | 502后票仍已消费；媒体恢复再消费404，PASS |
| 播放中 DELETE device | viewer/receiver结束、票失效、DB/API设备消失、bye计数增长、live_active=0，PASS |
| 删除后 REGISTER | 模拟器新注册 challenge 403，按预期退出1，PASS |

其余正常进程退出码均为0。signaling restart 场景故意暂停 simulator，期间两次 heartbeat 失败属于该故障注入，不是持续收流阶段错误。SSRC 与 delete/play 精确并发由 Go 定向测试补证，没有对真实 E2E 内部内存计数作未经验证的断言。

## 构建和失败记录

各阶段验证完成后才提交。最终：

- `go test -count=1 ./...`：PASS。
- `go vet ./...`：PASS。
- `go test -race -count=1 -timeout=30s ./...`：PASS。
- `cmake --build build -j12`：PASS。
- `ctest --test-dir build --output-on-failure`：14/14 PASS，22.65 秒。
- `git diff --check`、`git diff --cached --check`：PASS。
- ASan/UBSan 本轮未重跑：C++ 生产源码没有修改；不把 Go race 或 CTest 描述为 sanitizer 验证。

构建沿用本机既有 `PKG_CONFIG_PATH=/tmp/libsrtp-2.7-prefix/lib/pkgconfig:/home/gyl/ffmpeg901/lib/pkgconfig`。

失败证据没有覆盖/隐藏：

- 各阶段先执行 RED；旧路由测试先得到400而非404，清理后 GREEN。
- simulator 新等待循环曾复制含 atomic 的结构，vet 报错；按索引读取后 vet/race 通过。
- 首轮 E2E 的 async wait 测试脚本提前读取 Chrome stats；修正为等待实际 resolved boolean 后重跑通过。
- 首次 stage4 CTest 与真实 E2E 并行时，udp_transport 的 GB sender 启动失败。二者使用相同默认媒体端口范围；E2E 退出后隔离运行14/14通过，最终阶段也隔离运行14/14通过。没有修改产品端口策略来绕过冲突。
- 一次 legacy GREEN 命令误在仓库根目录运行，因无 Go module 未执行测试；随后在 signaling 模块目录成功执行。
- Stage3 最初 RED harness 未释放阻塞的外部 HTTP handler，SIGQUIT 终止并保留堆栈；修正测试 cleanup 后正常 RED/GREEN，未将其归因于生产 hang。

## 变化规模与 MVP 边界

按 `git diff BASE_HEAD CODE_HEAD --numstat` 统计：生产 Go/Web 代码增加722、删除960，净减少238行；Go tests 与独立 E2E 增加1286行。文档另计，媒体生产代码0改动。

明确边界：

- GB live 仅 UDP。
- 单一平台级 SIP password。
- channels、registration、live、play tickets 仅 runtime。
- 一次性播放 URL 仅 WHEP；没有改 RTSP/RTMP/HTTP-FLV/HLS 授权。
- 无自动 idle stop、viewer counting/refcount。
- 无 live/ticket restart recovery。
- 无 JWT/OAuth/Redis、generic proxy/resource framework、新状态机或兼容 API。
- 只证明自有 simulator/signaling/media_server 的本机 E2E；不声称真实厂商 GB28181 互操作已验证。

重跑方式：在 signaling 构建两个 Go 二进制，运行 `python3 bench/gb_signaling_verify.py --media <media_server> --signaling <signaling> --simulator <simulator> --output <新的空目录>`。不要与使用默认媒体端口池的 CTest 同时运行。
