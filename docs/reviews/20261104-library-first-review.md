# 全工程库优先审查与过度实现收口（2026-11-04）

## 结论

基线 `e26b4e0`（`origin/main`），工作区未提交。审查范围包括 C++ 媒体服务、Go 信令、Web 控制台和必要的测试工具。

这次只实施两项代码修改：

1. **撤销 UDP GSO**：6 个生产文件恢复为 `b131705` 的实现。
2. **SRTP profile 参数改由 libsrtp 提供**：删除三份手写映射表（服务端 DTLS、服务端 SRTP、压测客户端各一份）。

其余候选或者现有库没有等价能力，或者换用后会带来新的耦合或依赖，都只记录结论，不改代码。

撤销 GSO 接受的性能回退以 `20261103-udp-io` 报告中 base 与 gso2 的差值为准；本轮同条件复测确认撤销后与 base 相同：

- GB UDP：服务端 cycles 约 +86%；
- WHEP：+14%；
- 混合负载 D：+6%，健康容量从 2.4× 回到 2.2×；
- 混合负载 B：容量从 1.7× 回到 1.6×。

验证全部通过（矩阵中保留既有的 RTSP AAC XFAIL），见第 5 节。

结论标记：KEEP（保持现状）、SIMPLIFY（本轮精简）、REJECT（有替代方案但不采用）。

## 1. 范围与方法

- **依据**：项目 `AGENTS.md`、`.agent/` 五份文档、`docs/` 六份当前文档，以及 [20261031 媒体核心精简审查](20261031-media-core-simplification.md) 和 [20261103 UDP GSO 报告](../performance_reports/20261103-udp-io/report.md)。已裁决的事项（D01–D13、C01–C08，以及上次审查的 KEEP/REJECT 项）不重新讨论；本轮没有找到能推翻它们的新源码证据。
- **扫描方式**：
  - 第一方代码中的原始系统调用、throw/try/catch、队列与缓冲成员、全局表、自写编解码（hex、base64、CRC、RTP 头、NAL、ADTS）、超时与重试；
  - 逐项核对 `third/` 与 `deps/` 中的库实现。
- **"库是否等价"以实际源码为准**：例如 librtp 头文件声明了 `rtp_ext_write`，但源码里没有实现；Asio 1.92 的 resolver 不支持单操作取消；OpenSSL 的 hex 解析失败时会写入线程错误队列。

## 2. 逐项审查

| # | 模块 | 当前实现 | 复杂度问题 | 库替代方案 | 历史决定 | 结论 | 代码修改 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | `media/net/udp_transport.*`、`whep_session.cpp`、`gb28181_rtp_sender.*`、`gb28181_udp_sender_session.cpp`（UDP 发送） | `533a75e`：Boost.Asio 之外加线程局部暂存区、`cork/uncork`、跨 transport 持有者、原始 `sendmsg(UDP_SEGMENT)` 和 errno 回退；会话要配合 cork | 确定。为性能新增了状态和跨会话的隐式所有权，绕过 Asio 直接做 I/O，也要求上层协议会话配合 | Boost.Asio 的 `send_to` 加 `async_send_to` 就能满足原语义；Asio 不提供 GSO，等价的 GSO 只能自写 | 20261103 报告判 GO；本 Goal 按维护性取舍撤销 | **SIMPLIFY（撤销）** | 6 个文件恢复 `b131705`，−193/+18 行 |
| 2 | `dtls_transport.cpp` `profile_size()`、`srtp_transport.cpp` `set_crypto_policy()`、`bench/clients/webrtc_client.cpp` `make_client_srtp_material()` | 三处各写一张"profile 名称 → key/salt 长度或加密策略"的表 | 确定。同一张协议表在第一方代码里有三份，并且与 libsrtp 自带的表重复 | libsrtp 的 `srtp_profile_get_master_{key,salt}_length()` 和 `srtp_crypto_policy_set_from_profile_for_{rtp,rtcp}()` 按 `srtp_profile_t` 给出相同结果；OpenSSL 选中 profile 的 `id` 与 `srtp_profile_t` 同为 RFC 5764 注册值。逐项核对：`aes_cm_128_hmac_sha1_80` 是 `rtp_default` 的宏别名，RTCP 默认相同；GCM 两项一致；长度 16/14、16/12、32/12 一致 | 无 | **SIMPLIFY** | 见第 3 节 |
| 3 | `dtls_transport.cpp` `parse_sha256_fingerprint()`、`dtls_certificate.cpp` `make_fingerprint()` | 手写 `XX:XX` hex 解析和格式化，约 50 行 | 存在少量自写代码 | 有替代：OpenSSL 3 的 `OPENSSL_hexstr2buf_ex`/`OPENSSL_buf2hexstr_ex(':')`。但解析失败时会 `ERR_raise` 写入线程错误队列，而 `SSL_get_error` 会检查这个队列：一个无效 offer 会让同一 worker 上后续会话的 `WANT_READ` 被判为 `SSL_ERROR_SSL`，除非再加 `ERR_clear_error`。输入是不可信的 SDP，换用反而引入跨会话耦合 | 无 | **REJECT** | 无 |
| 4 | `media/webrtc/stun_message.cpp`（ICE-Lite STUN） | 自写消息编解码；CRC32 用 Boost.CRC，HMAC-SHA1 用 OpenSSL | 范围有限（497 行），只处理 ICE-Lite 需要的属性 | 现有依赖里没有 STUN 库；libnice/libjuice 是新依赖，并且会接管 ICE 状态，相当于替换协议栈 | D07、D08 | **KEEP** | 无 |
| 5 | `media/webrtc/webrtc_sdp.cpp` | 解析用 ireader 的 `sdp_parse`、`sdp_a_rtpmap`、`sdp_a_extmap`；自写 fmtp 键查找、H.264 profile-level-id 兼容匹配、H.265 匹配和应答生成 | 自写部分对应 WebRTC 协商规则 | ireader 的 `sdp_a_fmtp_h264` 只能把字段解析成结构，不做 profile-iop 掩码匹配；没有 Opus fmtp 解析，也没有应答生成器 | D08 | **KEEP** | 无 |
| 6 | `webrtc_packetizer.cpp` 的 MID 头扩展 | 用 librtp 的 `rtp_packet_serialize` 写扩展；扩展字节（one/two-byte）是自写的 | 约 15 行 | `rtp-ext.h` 声明了 `rtp_ext_write`，但 `third/ireader/librtp/source` 里没有它的实现，不能调用 | 无 | **KEEP** | 无 |
| 7 | RTP/RTCP 打包与解包、PS、FLV、TS、RTSP、RTMP 协议 | librtp、libmpeg、libflv、librtsp、librtmp；`whip_media_receiver` 只读 PT/SSRC，用于选择 demuxer | 无重复 | 已经在用库 | D01–D03 | **KEEP** | 无 |
| 8 | `media/codec/codec_utils.cpp` | Annex-B/AVCC/HVCC 转换、ASC、ADTS 都调用 ireader `mpeg4-avc/hevc/aac`；`length_prefixed_payload` 按需转换一次 | 无。缓存属于已接受的共享派生（D03、R4） | 已经在用库 | D03、R4 | **KEEP** | 无 |
| 9 | `media/codec/audio_transcoder.*` | FFmpeg、libfdk-aac、libopus | 复杂度来自时间线和 FIFO | 已经在用库；本 Goal 不改转码器 | 上次审查 | **KEEP** | 无 |
| 10 | `srtp_transport` / `dtls_transport` 主体 | libsrtp 加 OpenSSL 内存 BIO | 无 | 已经在用库 | D08 | **KEEP** | 只有第 2 项 |
| 11 | `media/http/signaling_verify.cpp` 的解析超时 | `async_initiate`，`async_resolve` 与 `steady_timer` 竞争 | 约 35 行自写取消 | `cancel_after`/`cancel_at` 依赖单操作取消；Boost 1.92 的 `resolver_service` 不实现取消槽，后台 `getaddrinfo` 只认 resolver 级的 `cancel()`，因此套用后超时不生效。Beast `tcp_stream` 只管 connect、read、write | D12 | **KEEP** | 无 |
| 12 | `gb28181_playback_session.cpp`、`gb28181_audio_session.cpp` 的 socket 换 worker | `release()` + `assign()`，失败时 `::close` | 两处各约 15 行 | Asio 没有跨 `io_context` 移动 socket 的接口，`release/assign` 就是官方做法；`::close` 只在 assign 失败时回收 fd | D06 | **KEEP** | 无 |
| 13 | TCP transport 写队列、HTTP-FLV 输出队列、RTMP/RTSP/GB TCP 会话的 `output_` | transport 保持单一串行队列；会话的 `output_` 只把一帧的包聚合后一次写出 | 不是第二个队列，没有额外重试或取消 | 已经用 Asio | D04、Goal 明确保留 | **KEEP** | 无 |
| 14 | `worker_sink_dispatcher`、`stream_registry`/`session_registry`、`whep_audio_egress` 弱表、`whip.cpp` 会话表 | 有界分发和按 identity 删除 | 每项状态都有独立事实 | 无对应库 | D03、D06、D07，上次 KEEP | **KEEP** | 无 |
| 15 | RTSP/RTMP/WHEP Pull 的 resolve、connect 及恢复检查 | Asio，恢复边界检查取消与 identity | 必要 | 已经用 Asio | D09、L02、L03 | **KEEP** | 无 |
| 16 | Go 信令 | sipgo、pion/sdp、icholy/digest、uuid、modernc sqlite；GB 应答先去掉 `y=`/`f=` 再交给 pion | 预处理是必要的：pion 不接受 GB28181 的非标准行 | 已经在用库 | D11 | **KEEP** | 无 |
| 17 | Web 控制台（`signaling/web/*.js`） | 浏览器 `RTCPeerConnection`、`fetch`，AudioWorklet 上传 | try/catch 是 JS 的 Promise 错误处理惯例，不属于 C++ 的无异常约定 | 已经用浏览器 API | — | **KEEP** | 无 |
| 18 | `bench/clients/webrtc_client.cpp` 的 STUN/DTLS | 独立于服务端的客户端实现 | 有意保持独立，避免测试用同一份代码自证 | — | — | **KEEP**（profile 表随第 2 项删除） | 第 2 项 |
| 19 | 原始系统调用 | 撤销 GSO 后，`media/` 只剩第 12 项的 `::close` | 无 | — | — | **KEEP** | 无 |

### 与异常约定不一致的第一方代码

| 位置 | 用途 | 能抛出的来源 | 结论 |
| --- | --- | --- | --- |
| `media/net/media_port_pool.cpp:35,40` | 构造函数 `throw std::invalid_argument` | 唯一调用者 `main.cpp:20` 传的是编译期常量 49152–65535，运行时不可达 | 保持。可选的最小修改：删除 `init` 参数，改用 `static_assert`。这项与库能力无关，属于规范收口，等你决定 |
| `media/rtmp/rtmp_session.cpp:134,151`、`media/rtsp/rtsp_server_connection.cpp:209,276` | ireader C 回调边界上的 catch-all | 回调内没有 `stoi`/`at`/`optional::value`、不带 `error_code` 的 Asio 重载或其他会抛异常的接口，实际只兜底分配失败 | 保持。去掉后，分配失败会在 C 栈帧上 terminate，行为会改变；需要单独决定 |
| `media/http/signaling_verify.cpp:63,178` | catch-all | 同上；内部 I/O 全部使用 `yield[error]` | 保持，同上 |
| `config.cpp:58,73` | `boost::program_options` 解析失败 | 这个库只通过异常报错 | KEEP（Goal 规定不为此换成更复杂的命令行解析） |
| `bench/*.cpp`、`bench/clients/gb_client.cpp`、`bench/*.h` | 工具的参数校验和运行错误 | — | KEEP（测试工具） |
| `signaling/web.go:15` | `fs.Sub` 失败时 panic | 嵌入文件系统的编译期内容，运行时不可达 | KEEP |

## 3. 代码修改

### 3.1 撤销 UDP GSO（第 1 项）

`media/net/udp_transport.{h,cpp}`、`media/webrtc/whep_session.cpp`、`media/gb28181/gb28181_rtp_sender.{h,cpp}`、`media/gb28181/gb28181_udp_sender_session.cpp` 恢复为 `b131705` 的版本（`git checkout b131705 -- <文件>`）。`git diff b131705` 在这 6 个文件上只剩第 3.2 节在 `whep_session.cpp` 中改的一行日志。恢复后的行为：

- 队列为空时直接同步 `send_to`；
- 返回 `would_block` 时进入 1 MiB 高水位的有界队列，由 `async_send_to` 逐个发出，completion 捕获 datagram 保活；
- 同步错误 post 到错误回调，异步错误直接回调；
- `shutdown()` 清空队列、清掉回调并关闭 socket；会话决定何时关闭。

没有改用其他批量发送方案。

### 3.2 SRTP profile 参数改由 libsrtp 提供（第 2 项）

- `dtls_srtp_keying_material::profile` 的类型从 `std::string` 改为 `srtp_profile_t`。
- DTLS 侧用 `SSL_get_selected_srtp_profile()->id` 转换出 profile，再用 libsrtp 的长度函数切分导出的密钥；删除 `srtp_profile_size` 和 `profile_size()`。
- SRTP 侧用 `srtp_crypto_policy_set_from_profile_for_{rtp,rtcp}` 设置策略；删除 `set_crypto_policy()`。
- 压测客户端的 profile 表也换成同样的库调用。
- 握手完成日志仍打印 OpenSSL 的 profile 名称；其余 debug 日志改为打印数值 id。
- 协商的 profile 列表（`SSL_CTX_set_tlsext_use_srtp`）、密钥布局和重放窗口都没有变。
- 行数：5 个文件（含 `whep_session.cpp` 的一行日志）+28/−90。

## 4. 撤销 GSO 接受的性能回退

### 4.1 历史数据

数据取自 [20261103-udp-io](../performance_reports/20261103-udp-io/report.md) 第 6 节，原始数据未修改。base 与当前 UDP 实现逐行相同。

| 负载（8 核） | GSO（gso2） | 撤销后（base） | 回退 |
| --- | --- | --- | --- |
| GB UDP 4000 | 5.18 Gcyc/s | 9.63 Gcyc/s | +86%（每数据报约 4.8k→8.8k cycles） |
| WHEP 1536 | 7.37 | 8.41 | +14% |
| 混合 D 2.2× | 16.90 | 17.91 | +6%，健康容量 2.4×→2.2× |
| 混合 A 1.0× | 15.73 | 16.37 | +4%，容量不变 |
| 混合 B 1.6× | 17.21 | 17.65 | +3%，健康容量 1.7×→1.6× |
| 混合 C 2.15× | 17.20 | 17.35 | +1%，容量不变 |

此外，GB 4000 的积压延迟 lag 从 122–134 ms 回到 174–226 ms，控制面 p99 从 31–58 ms 回到 117–129 ms；WHEP lag 从 11–15 ms 回到 25–31 ms。PSS 不变。

### 4.2 本轮同条件复测

同一测试机、同一负载和素材，新版本（lib，工作区）与 base（`b131705` 产物）交替运行，每组 2 次，12 次全部 PASS HEALTHY。指标为服务端 60 s 窗口的 cycles 和 instructions（schedutil 下 CPU 时间不可比）。

| 负载（8 核） | lib Gcyc/s | base Gcyc/s | instructions（lib / base） | 判断 |
| --- | --- | --- | --- | --- |
| GB UDP 4000 | 9.73 / 9.63 | 9.69 / 9.67 | 约 649–657 G，两组相同 | 相同；与 4.1 节 base 的 9.63 一致 |
| WHEP 1536 | 8.74 / 8.55 | 8.35 / 8.38 | 661.4–662.5 G，两组相同 | cycles 高约 3.4%，指令数相同 |
| 混合 D 2.2× | 17.94 / 17.97 | 17.96 / 17.90 | 约 1445 G，两组相同 | 相同 |

WHEP 的 cycles 差异只有 2 次样本，并且指令数相同，更可能来自频率波动。第 2 项只改 SRTP 会话建立时的策略选择，逐包 protect 路径没有变，因此不能把这个差异归因于代码。server_cores 为 5.77 对 5.76。

结论：撤销后的性能与 `b131705` 相同，接受的回退就是 4.1 节的数值。

## 5. 验证

| 项目 | 构建 | 范围 | 结果 |
| --- | --- | --- | --- |
| 本地构建 | Release、ASan+UBSan、TSan | 全部目标，0 警告；clang-format 无差异 | PASS |
| SRTP profile 互通（`srtp_profile_check`） | rel / asan / tsan | 服务端为生产 `dtls_transport` + `srtp_transport`，客户端为只提供单一 profile 的 OpenSSL 加上按显式策略建立的 libsrtp。三种 profile 各检查：握手选中该 profile、RTP 双向各 50 包、RTCP 双向、篡改包被拒绝。改动前（HEAD 文件）与改动后都是 24/24 | PASS |
| UDP 发送（`udp_transport_check.sh`） | rel / asan / tsan | 81 个大小混合（含 0 字节）的数据报发往两个端点，按序完整到达；strace 记录 86 次 `sendto`、0 次 `sendmsg`/`sendmmsg`；5 次 EINVAL 走错误回调；netns tbf 限速下高水位拒绝，已接受的数据报全部到达；带队列关闭 | PASS |
| 协议矩阵 | Release | 35 个场景：21 PASS HEALTHY、11 PASS UNCONFIRMED（故障注入场景预期 exit=1）、3 XFAIL（`rtsp-{tcp,udp}-h264-aac`、`rtsp-tcp-h265-aac`，RTSP AAC 锯齿，严格 XFAIL/XPASS 规则），0 FAIL、0 XPASS | PASS（含 3 XFAIL） |
| 协议矩阵 | ASan+UBSan | 22 个场景子集：17 PASS HEALTHY、3 PASS UNCONFIRMED、2 XFAIL；30 份 sanitizer 输出都只有 makecontext 提示，无 ERROR | PASS（含 2 XFAIL） |
| 协议矩阵 | TSan | 12 个场景子集：10 PASS HEALTHY、1 PASS UNCONFIRMED、1 XFAIL；无报告 | PASS（含 1 XFAIL） |
| Chrome WHEP 边界（WHIP/Pull × video/PCMU/AV、G.722、FLV 伴随） | rel / asan / tsan | 所有 Chrome 观众 connected，约 30 fps，freeze/drop/lost 均为 0；fanout 6 路无运行时失败；FLV 无音频 PTS 缺口；WHEP 12 启动 12 关闭，fd 回到初始值；服务端 exit 0 | PASS |
| WHEP 生命周期 churn | rel / asan / tsan | PASS HEALTHY；无 sanitizer 错误 | PASS |
| GB 生命周期 churn | rel / asan / tsan | PASS HEALTHY；无 sanitizer 错误 | PASS |
| 性能复测 | Release | 见 4.2 节 | PASS |
| Go | — | `go build`、`go vet`、`gofmt` 无输出；`go test` 没有测试文件 | PASS（`go test` 不计） |

协议覆盖包括 WHIP/WHEP、GB28181（UDP/TCP 主动和被动）、RTMP、RTSP（TCP/UDP）、HTTP-FLV、HLS，以及故障注入（无效或重放 token、源停止）、慢读者和跨 worker 多源场景。项目没有注册 CTest，因此没有把 CTest 当作验证依据。原始结果在测试机 `runs9/l2-*`、`runs9/*_check-*`。

## 6. 未解决问题

- WHEP SRTP `replay_fail`：上一轮已记录、早于 GSO 就出现过的问题。根因仍未确定，本轮不处理。
- 第 2 节异常表中的两项可选收口（端口池改 `static_assert`，以及删除 C 回调边界的 catch-all）都会改变错误处理行为或接口，没有实施，等你决定。
- `go test` 没有测试文件（`[no test files]`），不能算作 Go 测试通过；Go 只验证了构建、`go vet` 和 `gofmt`。

## 7. 文件

- [`../verification_results/20261104-library-review/`](../verification_results/20261104-library-review/)：
  - `build_check.sh`：用指定构建目录的 `libmedia_core.a` 编译检查程序；
  - `srtp_profile_check.cpp`：三种 SRTP profile 的独立参考实现互通检查；
  - `udp_transport_check.{cpp,sh}`：同步发送路径检查，覆盖顺序、错误回调、tbf 背压与高水位、带队列关闭；
  - `jobs/*.sh`：测试机上运行的任务。
- 原始结果只保存在测试机上的 `runs9/`。
