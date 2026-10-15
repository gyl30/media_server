# 阶段四：有限录像传输子集

本子集在 `feature/qt-monitor-client` 上实现独占的 TCP/RTP-PS → HTTP chunked 通道。它不是完整历史回放验收，不改变实时媒体核心，也没有将测试用 verifier 冒充真实 signaling 历史授权。

## 职责与边界

- 控制面 `POST /gb28181/receiver/playback/create` 复用已有强类型 receiver 配置，只接受 `tcp_passive`。当前请求仅有 `stream_id/transport/payload_type/ssrc` 四字段，服务端复用媒体端口池分配 TCP 端口，201 返回 `{"rtp_port": P}`。沿用控制接口授权，不新增数据库、端口池或 source registry；详见后面的动态端口增量。
- `GET /play/gb28181/<stream_id>/<token>.ps` 只接受 HTTP/1.1 GET；拒绝 query、body、无效标识及解析时已经缓冲的多余字节。先固定本地 receiver，再调用既有 `verify_stream()`。成功认领永久消费，后续失败不恢复。
- 该 receiver 只允许一个消费者，不注册 `media_stream`，不增加 GOP/history、转码、PS muxer 或跨 worker 媒体队列。未 attach 时只监听，不读取设备数据；它的绝对回收期限由后续 signaling 历史会话 owner 管理，不借用或修改实时 receiver 的 20 秒 idle。
- HTTP socket 原来的 executor 不会因 move 自动变化。交接在目标 worker 通过 native handle 的 release/assign 完成，scope exit 保护尚未接管的 fd。临时 work guard 保证已投递交接在 worker 排空前执行；依赖当前 worker pool 先 join 再销毁 contexts、正常停止不用 `io_context::stop()` 的模型。
- 所有接收、写入、订阅清理和关闭在 receiver owner worker 上执行。一次只持有一个长度最多 65,535 字节的 RFC 4571 datagram；每个 PS chunk 完成写入后再读下一包，慢消费者通过 TCP 反压，不丢旧包、不增加无界应用队列。
- 校验公开 RTP parser 的结果、PT、SSRC、非空载荷和连续序号（含 wrap）。当前专用通道不识别夹带的 RTCP；真实设备是否复用 RTP/RTCP 尚未验证，不能据此声称通用 TCP 录像兼容。
- 每次 HTTP 写有 20 秒期限。独立的一次消费者读检测断开及不应出现的新输入；消费者 half-close 写方向也视作离开。Qt Network 当前用法不会采用该 half-close。

## 结束事实与清理

设备在完整 RFC 4571 报文边界 FIN，且至少收到一个有效 RTP 包时，才发送 HTTP 终止块。半长度、半载荷、序号缺口和其他错误直接关闭，不发送成功终止块。

这只证明**已收到的传输正常结束**，不能证明录像库存完整、请求时间段完整，或设备已经发送全部预期帧。实验中提前在完整 PS pack/RTP 边界 FIN，客户端合法 EOF，但只有 25/50 帧和一半 PCMA，必须保留这一限制。SIP 121、远端 BYE、设备录像能力和内容完整性属于后续独立事实。

`shutdown()` 总是 post 本对象的 `safe_shutdown()`；后者按 identity 摘除注册，解除 shutdown subscription，关闭监听、设备及消费者 socket。保留 `consumer_` 对象直到在途协程结束，避免 Beast 写操作仍持有引用时提前销毁 stream。没有新增关闭、挂接或启动标记；重复清理不影响同名替换对象。

## 已执行的组件证据

DEV 独立证据目录：`/var/tmp/media-server-qt-finite-ps-kLBXkM/`。GCC 实际版本为 16.0.1 experimental；正常服务端使用既有静态 FFmpeg SDK。独立 sanitizer 构建复用已验证的 Boost 1.92 ASan 前缀，原有 CMake 兼容守卫真实通过，启用 ASan、UBSan 与泄漏检测。

| 证据 | 实际范围与结果 |
| --- | --- |
| `logs/playback-test-normal-03.log`、`logs/playback-test-asan-02.log` | 初始 12 个传输生命周期场景通过：跨 worker、来源 worker 停止后继续接收、序号 wrap、截断、空输入、消费者断开、重复消费者、替换 identity、worker stop、weak 最终释放 |
| `logs/playback-final-normal-03.log`、`logs/playback-final-asan-03.log` | 补充定向 normal 与 ASan+UBSan 各 2/2 CTest：17 个传输场景及 3 组 HTTP 入口用例。补充传输中关闭、owner stop、消费者 half-close、空 RTP payload、真实 20 秒慢客户端写期限。HTTP 26 次请求覆盖本地前检、缺失/错误 receiver、拒绝后成功及重放；前检不调用 verifier，成功后一次性消费。后续局部 accept 失败回收修正由最终完整套件覆盖 |
| `logs/server-final-normal-ctest-02.log` | 最终正常 RelWithDebInfo 完整构建、串行 CTest 69/69 通过，456.57 秒；编译保留 `-O2 -g -DNDEBUG -Werror`，测试明确使用 `/home/gyl/bin/ffmpeg` 7.1.1 |
| `logs/server-final-asan-ctest-02.log` | 修正既有 accept 故障注入工具后，ASan+UBSan 完整串行 CTest 69/69 通过，458.31 秒；`detect_leaks=1`，sanitizer 错误不恢复。完整覆盖最终局部 accept 失败清理版本，没有排除失败项 |
| `server-media-normal-02/result.json`、`server-media-asan-01/result.json` | 各 12 项组件断言通过，实际服务端双 worker → Qt Network custom AVIO → FFmpeg 解码；每项 receiver 自然注销，服务 SIGTERM 退出 0，无 sanitizer 错误 |
| `server-media-final-normal-01/result.json`、`server-media-final-asan-01/result.json` | 用预先冻结的最终服务端 ELF 重新验证，各 12 项组件断言通过、receiver 自然注销 12/12、SIGTERM 退出 0，无 sanitizer 错误。结果 SHA 分别为 `f865ec3046a65e5c1a583f63b3081374f21f725bf04c21dc6c614a5eda85ea10`、`4dbedea6db8ecef1dbe31378bba9a3cda2fff328eab4d2eb75bafb9046479e0f` |
| 完整媒体 8 项 | H.264 与 AAC/PCMA，音视频先后顺序及 PS end code 有无；逐包 PS SHA、逐帧 luma 指纹与参考相同。50 帧/1 IDR，PCMA 16,000 samples/channel，AAC 97,280 samples/channel；AAC 数量以编码后参考为准 |
| 异常媒体 4 项 | 半长度、半载荷、序号缺口被识别为网络失败；提前完整边界 FIN 记为 `EXPECTED_TRANSFER_ENDED_NOT_COMPLETE`，不是完整回放 PASS |
| `decode-matrix-qt-v4-01/result.json`、`tcp-matrix-v2-asan-01/result.json` | 私有 HTTP/解码 64 项及 TCP/RTP 12 项组件断言通过，严格检查进程退出及 sanitizer 输出；不是 GUI 回放验收 |

Qt、FFmpeg、ireader 等外部依赖未全部插桩，不能将第一方 ASan 结果扩大为所有第三方库的完整内存安全证明。当前媒体组合使用明确标注的 fake `/internal/verify`；真实 Go 历史票据尚未实现。

慢客户端用例在 TCP 握手前设置 64 KiB 接收缓冲，先确认健康正文，再停止读取。服务端 normal 在 20,001 ms、ASan 在 20,002 ms 自行关闭；设备分别完成 140、105 个报文，注册表已自然移除。恢复读取后分别用 5、17 ms 排空已收到的 2,668,939 字节，解析为 `partial_message`，没有 HTTP 正常终止块。失败兜底期限仍为 30 秒，排空期限仍为 10 秒，没有修改生产 20 秒期限。设备成功写入 TCP 的报文数不是服务端已经接收/交付的报文数。

ASan 运行保留 `makecontext/swapcontext` 支持不完整的 runtime warning；没有抑制报告或关闭泄漏检测，不能把通过解释为 sanitizer 对所有协程栈操作的完全证明。

最终 ELF 位于证据目录 `final-transport-elf/`，正常版 SHA 为 `63fed2636f74a0e9ec8b5ec0cc9c8493720caaa9079c005eee3714908e0d3cff`，ASan 版为 `1ed77ff52fd2a38cd720e0e5d0aaa277b66f5d54371505949585cc65e66f6456`。参考素材清单 `reference.jsonl` SHA 为 `9516756edb51c09683a17b0dae63d9534204962b207e41dbb5e8af3ddbe32aba`。旧 `normal-02/asan-01` 首轮运行的 ELF 后来重链接覆盖，旧 metadata 和结果保留，但原 ELF 没有及时冻结；本轮明确以新冻结路径和最终新结果建立可复核对应，不冒称找回旧二进制。

### 保留的失败与修正

- 独立探针早期异常构造泄漏 1,696 字节，最小构造失败复现旧版 LSan 失败、新版通过；修复的是私有验证工具，不是生产缺陷。早期负例不检查进程退出的问题也已修正，不能沿用旧矩阵宣称 sanitizer 通过。
- 生命周期夹具曾在 owner worker 已退出后再次 post shutdown，导致 weak 断言失败。保留原失败日志，仅修夹具停止后的清理请求；没有改生产关闭语义或放宽 weak 断言。
- 新中断用例还曾在 HTTP body 已消费 TCP EOF 后再次等待同 socket EOF。原始失败、关键步骤日志和 `logs/playback-final-normal-failure-strace-01.log` 保留；锁定 Boost 1.92 的 epoll `done_and_exhausted` 路径会使第二次读等待新的边沿。最终仅改测试为严格验证第一次 `partial_message`/EOF/reset，不重复观察已消费的 EOF；仍检查无终止块、健康正文、设备关闭、注册表和 weak 释放，没有修改依赖或生产实现。
- 慢客户端旧夹具在连接后骤缩接收缓冲至 1,024 字节，导致已在途的 32 KiB segment 无法正常进入骤缩窗口，出现重传和退避。`logs/playback-final-normal-02.log`、`logs/playback-slow-socket-01.ss` 保留自然 20 秒关闭通过但后续排空超时的事实。改为握手前设置 64 KiB，只修夹具，不延长预算、不移除实际读取无终止块的断言；不是生产缺陷的 RED/GREEN。
- 测试私有 accept listener 增加局部 scope-exit：setup 失败时也 post 到 owner 关闭，避免原始 `async_accept` 未被取消而阻塞夹具 join；不新增生产状态。weak 断言证明最终受控 shutdown/worker 排空后释放，不扩大为自然结束瞬间即析构。
- 完整回归首轮 `logs/server-final-normal-ctest-01.log` 为 64/69，通过项不掩盖五项失败。其缓存误选 `/usr/bin/ffmpeg` 导致 HEVC/FLV fixture 无法生成，另外四个并行 WHEP 用例争用默认媒体 UDP 端口。后续明确选择 `/home/gyl/bin/ffmpeg` 并串行执行；没有放宽断言、排除测试或改生产端口策略。
- ASan 完整首轮 `logs/server-final-asan-ctest-01.log` 为 68/69，唯一失败是既有 `accept_errors` 工具在子服务启动前把 fault-injection 库置于 ASan runtime 之前。独立原始启动复现 `logs/accept-fault-asan-startup-red-01.stderr`、`.exit` 保存运行库顺序诊断及退出码 1，不是有限传输组件的内存错误。只修测试：通过实际 ELF 的 `ldd` 确定动态 ASan 库，仅在子服务环境将其置于故障库之前；不改父 Python 环境、不禁用 sanitizer 检查或泄漏检测。保存子服务输出并要求正常退出，避免退出期 LSan 失败被工具忽略。`logs/accept-normal-green-01.log`、`logs/accept-asan-green-01.log` 分别通过，3.53/3.56 秒，均在真实注入错误后 3 秒恢复且服务退出 0；完整 ASan 重跑 69/69 的证据单列于上表，不以定向结果冒充全量。
- 私有真实媒体 runner 首次启动前因局部变量遮蔽 `http` 模块失败，保留 `server-media-normal-01` 及日志，最小重命名后另开 `normal-02`，不覆盖失败。
- Qt v1 清单原来指向临时借用的文件名；原文件名已恢复对照实现。原始清单保留，新增 `qt-v1-manifest-path-correction.md` 和 `SHA256SUMS.qt-v1-frozen-paths`，只映射已冻结的相同内容 SHA，不改写旧结果。

## 动态 TCP 端口增量

初始提交 `f528737` 的显式 `listen_port` 仅用于先验证原始有限通道；本增量移除该历史入口，不保留兼容模式。普通 GB TCP create 仍要求调用方给出非零 `listen_port` 并返回空 201，普通 UDP 仍返回 `rtp_port`，不改变其他收发接口。

`gb28181_receiver_kind` 是解析/创建调用中的模式参数，不持久保存。同一解析器只解析一次共同四字段；历史模式拒绝 `listen_port`、主动 TCP、UDP 及其他多余字段。

唯一新增运行资源成员是 `optional<uint16_t> local_port_`：`startup(address)` 内 acquire，失败返回空，由外层原有 `shutdown()` 回收，不试池中的下一项。accept 后监听 socket 已关闭而设备连接仍使用 P，因此租约不能从 listener 是否打开推导。`safe_shutdown()` 先关闭实际 socket，再归还并清空租约；否则旧对象重复关闭会误归还新对象已经复用的 P。没有新增关闭标记、锁、timer 或端口管理层。池按原有 P/P+1 分配单元保留，TCP 只绑定 P，不改池算法。

当前增量证据仍在同一独立目录，未覆盖初始提交的 runner、结果或冻结 ELF：

| 证据 | 实际范围与结果 |
| --- | --- |
| `logs/pool-normal-directed-01.log` | 2/2 CTest，20.05 秒；19 个传输生命周期场景和 5 组 HTTP、41 次请求 |
| `logs/pool-asan-directed-01.log` | 相同场景 ASan+UBSan、LSan 启用，2/2 CTest，20.16 秒 |
| `logs/pool-normal-full-build-01.log`、`logs/pool-asan-full-build-01.log` | 本增量正常和 ASan+UBSan 的全部目标构建成功，不复用前一提交的构建结论 |
| `logs/pool-normal-full-ctest-01.log` | 本增量正常构建完整串行 CTest 69/69 通过，456.20 秒 |
| 本增量完整 ASan CTest | NOT_RUN；仅新定向 2/2 和新冻结 ELF 的实际媒体 12 项通过。前一提交完整 ASan 69/69 的历史结果仍保留，但不冒充本增量已重跑全量 |
| 端口生命周期 | 原 17 场景保持；每次最终清理后池 4 项全部可取回、旧端口可 bind。新增池耗尽、真实 TCP bind 冲突不重试、失败后回收、新 owner 复用后旧 owner 多次关闭不归还新租约 |
| HTTP 契约 | 保留原 26 请求，新增 15 请求；实际控制授权、四字段及显式端口拒绝、TCP 连接、普通 TCP 空 201、普通 UDP 响应与实际 RTP/RTCP bind 占用对照。两个测试进程使用各自小池，不共享固定监听端口 |
| `server-media-pool-normal-01/result.json`、`server-media-pool-asan-01/result.json` | 正常/ASan 各 12 项组件断言通过，8 份双轨 PS 与 4 种异常条件，逐帧/PCM/PS 验收不变；各自 receiver 自然注销 12/12，服务退出 0，无 sanitizer 错误 |

新媒体 runner 为 `run_finite_playback_server_probe_v2.py`，SHA `e5ec5b9f67c0361075c8bb881fec89b12e0ae9a351c7204eafdb56c795099c07`，仅改为读取严格 201 `rtp_port`，本轮服务绑定 `127.0.0.2` 隔离，不修改系统网络。新冻结 ELF 在 `pool-transport-elf/`，normal SHA `1d1fca587eb1cb12c96eae1c668ecd3e80aa824ecc6a1873d3b426ea819cd16a`，ASan SHA `3512c4489f16e3fe4c182821a53252ad01b2301c5125f64bfc665c74a36138eb`。两个结果 SHA 分别为 `f0a828d9b1d390e5791331be2eba7a69a9cd9cc9548676de70de0ed02cad56e4`、`82c49fd73d1ea8f1488c8dc07f350d7c3d1ce080bb2b56c1a8ed5d6cb39802df`。

Claude Code 第 44/45 轮完成设计与代码只读复审，无阻断；不把讨论替代实际测试。新端口路径的全量 CTest 结果单独记录，不将前一提交的全量 69/69 自动继承为该增量的全量 PASS。

端口池耗尽、真实 bind 失败及旧对象重复关闭有直接测试；没有单独强制注入 socket open、listen 或订阅失败，这些出口的归还行为由共用失败返回和 owner 清理路径审查确认，不冒称已注入所有内部失败。

## 后续仍须完成

1. signaling 独立历史会话 owner、一次 60 秒播放票据、GET attach 后 INVITE、绝对期限和停止/失败回收。
2. `Playback` SDP 与时间范围、SIP 121/BYE/FIN 各自事实、INFO 序列及 PAUSE/恢复/seek。
3. Qt 有限输入模式及有界背压；保留探测缓存，codec/SWR flush 后继续显示与音频排空。当前实时 decoder/GUI 会丢旧帧及立即清音，不能只换 URL 就宣称完整录像播放。
4. 启用 MPEG-PS 的独立桌面 SDK、尾帧/短音频实际呈现、暂停/取消/关闭与运行包验证。
5. 真实录像设备的 TCP、121、暂停恢复及 RTCP 复用兼容性。当前这些设备端行为为 BLOCKED，不以受控 peer 或静态 RTP 替代。

因此：本传输子集通过不等于阶段四或整个 GB28181 平台 GO。

Claude Code 第 40 轮复审有限传输代码、第 43 轮复审 accept 工具修复均无阻断；提交门槛为以上完整构建、完整 normal/ASan 套件和冻结 ELF 的最终媒体结果。讨论原始记录在 DEV `/tmp/qt-client-plan-claude-JuZHjy/round-40.json`、`round-43.json`，不把只读讨论本身作为测试 PASS。
