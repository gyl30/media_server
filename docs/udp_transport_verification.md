# UDP transport 统一验证记录

本轮将 UDP datagram 的串行写入、endpoint 和 buffer ownership 收口到 `media/net/udp_transport.*`。WHEP、WHIP、GB28181 UDP sender/receiver 和 RTSP UDP publish 已迁移，协议读循环、定时器间隔、packetization、端口分配策略保持原状。

`BASE_HEAD=c25092bf1ad1e22e253b8a37eba3ce532a287657`。开始时完成 fetch，工作树干净，HEAD 等于 origin/main。最终代码与测试版本为 `a8982d9e3900898569684294c7141efb503a0495`；其中生产源码最后修改于 `f572de3f8929c10b01b48b4973785df4a2859d33`，之后只补充验证工具与本报告。FINAL_HEAD 为本报告所属提交，可用 `git log -1 --format=%H -- docs/udp_transport_verification.md` 获取。

权威基线见 [稳定性验证](stability_verification.md) 和 [三轮性能基线](performance_baseline.md)，原记录未覆盖或修改。本轮机器可读汇总为 [udp_transport.json](verification_results/udp_transport.json)。原始日志、命令、结果与最终 Git 核验位于 `/tmp/media_server_udp-c25092b`。旧证据目录 `/tmp/media_server_verify-728178f` 的 fixture 和日志保留；其中构建目录增量重编当前源码，新的测试输出全部写入新目录。

提交列表：

| SHA | 提交信息 |
| --- | --- |
| 52f0c95b892312933ac6424bc6004401a4816a56 | 统一 UDP 传输命名与共享持有 |
| 1bd5b808cb4eaa56e30d4927f7a5582d45ca7060 | 统一 UDP 串行异步写入并接入 WebRTC 与 RTCP |
| 43440c8a413c5a55434f271750e171e2353c8896 | 接入 GB28181 UDP 写队列并删除会话调度 |
| 44ea582dcea3c1647168e46785e4786549f6a437 | 补充 UDP 写队列生命周期与会话策略测试 |
| f572de3f8929c10b01b48b4973785df4a2859d33 | 区分 UDP 入队拒绝与实际发送错误 |
| a8982d9e3900898569684294c7141efb503a0495 | 补充 RTSP UDP 发布与 GB RTCP 真实协议验证 |
| 本报告所属提交 | 记录 UDP 传输统一验证与性能比较 |

每阶段独立完成 `git diff --check`、`cmake --build build -j12`、全量 CTest、暂存检查、提交与 push。改名与共享持有先单独构建；随后删除旧发送 API 并迁移 WebRTC/RTCP；下一阶段删除 GB RTP 会话队列。没有保留两套 transport 或内部 API 兼容层。

## 1. 原 UDP 写路径结构

WHEP 在 session 中维护 pending datagram、UDP 写队列、字节计数和 start/completion 调度。WHIP 与 GB UDP sender 另外维护队列，并通过 spawn/yield 写循环发送。GB receiver、GB sender 的 RTCP 和 RTSP UDP publish 也在 session 中通过 yield 发包。socket、queue、发送 buffer 与关闭时序分属不同对象。

## 2. 新 udp_transport 结构

`udp_transport` 同时持有 socket 和唯一通用 datagram 写队列。每项为 `shared_ptr<pending_datagram>`，包含完整 `vector<uint8_t>` 与 endpoint。仅从空队列加入首包时启动发送；completion 处理当前项后启动下一项。没有新增 writer-active、ready 或 started 状态。

一项只对应一次 `async_send_to`，不合包、不拆包。endpoint 按值保存在每项中，socket 调用 connect 后仍支持逐包选择目的地址。读取继续使用原来的 yield-based `async_receive_from`。

写接口同步返回 bool 表示入队是否成功；span 复制数据，vector 转移数据 ownership。所有操作由 owner executor 调用，调用方沿用既有 worker 分发与 fencing。同步入队结果使 GB 的 `rtp_onsend()` 仍发生在接受入队之后，也让 overflow 与真实 socket error 清楚分离。没有为形式一致额外增加一次 post 或无用回调。

## 3. 删除的 session write queue/state

删除 WHEP/WHIP 的 `pending_datagram`、`udp_write_queue_`、`queued_write_bytes_`、`start_udp_write()`、`handle_udp_write()`、`run_udp_write()` 和会话高水位常量；删除 GB sender 的 `write_queue_`、`queued_write_bytes_`、`run_rtp_write()` 和高水位常量。移除相关 spawn/yield 写调度。

GB receiver 和 RTSP UDP publish 的 RTCP 直接提交 datagram。RTSP 的 `send_rtcp(track_index)` 只负责依次生成各 track 的报告，completion 后选择下一 track，保留协议顺序，不持有通用网络队列。

## 4. 保留的协议状态及原因

WHEP 的 `shutdown_requested_` 原子状态继续隔离跨 worker 关闭与待执行媒体任务。保留 ICE/STUN/nomination、DTLS/SRTP、remote endpoint、计时器及协议生命周期。

GB sender 的 `rtp_onsend()` 和 `rtcp_reporting_started_` 保留原统计与首次 RTP 后启动 RTCP 的时序；独立 RTP/RTCP socket、25 秒 sender RTCP 间隔不变。GB receiver 和 RTSP 的 1 秒 RTCP 间隔、peer 选择/锁定、读循环和关闭策略不变。没有修改 TCP transport、media_stream、worker_context、packetizer 或其他冻结业务架构。

## 5. queue high-water 行为

统一高水位为 1 MiB，计数包含正在发送的 datagram。恰好达到 1 MiB 可接受；加入新包后将超过上限则拒绝。先比较包大小，再用 `high_water - size` 比较已有字节数，避免加法溢出。completion 减去当前完整 datagram 的大小。

## 6. UDP overflow 行为

overflow 丢弃最新完整 datagram，返回 false，不进入队列，不改变已有队列或计数，不触发实际发送 completion。WHEP、WHIP、GB RTP 记录原有队满日志并继续 session；GB RTP 不为拒绝的包执行 `rtp_onsend()`。RTCP 入队拒绝时保留原来的后续报告/定时器推进。

overflow 不使用真实发送 callback 中的 `no_buffer_space` 表示，避免将内核返回的同名 socket error 当成媒体 drop。transport 不决定 session 生命周期。

## 7. socket error propagation

callback 仅报告实际 datagram completion 的 error/bytes。发送错误将 transport writer 置为 terminal，清理队列、计数和 callback，然后调用已保存的 callback 一次。各 session 根据既有策略处理真实 socket error 并 shutdown；transport 不 retry、不认识协议。

session callback 使用 weak_ptr，不形成 transport/session ownership cycle；成功 completion 不必锁定 session。shutdown 后取消产生的旧 completion 被 transport fencing 忽略，不再次向已关闭 session 报错。

## 8. shutdown/in-flight buffer lifetime 设计

`async_send_to` handler 独立捕获 `self` 和当前 datagram 的 shared_ptr。即使 shutdown 清空整个 queue，已提交操作所引用的 vector 和 endpoint 仍由 handler 持有，直到 completion 返回。这是 ownership 保证，不依赖 cancel 同步完成。

shutdown 设置 stopped，清 queue/callback/字节数，cancel 并 close socket。completion 在 callback 前后检查 stopped、队列是否为空及 front 是否仍为当前 datagram，允许 callback 重入 write/shutdown，也阻止 port pool 绑定重试后的旧 completion 触碰新队列。callback 自身临时共享持有，可在重入 shutdown 清除成员 callback 后安全返回。

## 9. targeted tests

新增公共边界测试 `tests/udp_transport.cpp`，注册 CTest `udp_transport`：

| 要求 | 实际验证 |
| --- | --- |
| A 串行与 buffer | 连续 40 个不同长度/内容 datagram，核对接收与 completion 顺序；span 入队后修改调用方 buffer，交替验证 move vector |
| B endpoint | 两个真实 UDP receiver，按 A/B/A 连续投递；改变调用方 endpoint；已 connect 的 socket 仍向不同 endpoint 投递 |
| C 关闭在途操作 | 100 轮，每轮入队 3 个 60,000-byte 包，首包 async 已提交后立即 shutdown 两次；未提交的后两包不能发出；关闭后 callback 清除，transport 在 completion 后释放 |
| D overflow | 32 × 32,768-byte 恰好 1 MiB 全接受；再加 1 byte 和超大包均拒绝；原 32 包无损，completion 中重入发送第 33 包成功 |
| E socket error | 65,508-byte IPv4 包触发真实 message_size，错误 completion 一次，后续队列不发送；关闭/重新 startup 后旧 completion 不干扰新包 |
| 会话 policy | 真实 GB sender 发送足以填满队列的 H264/PS/RTP，overflow 后 registry 仍在且后续小帧到达；目的端口 0 的真实发送错误移除 session，shutdown 一次并释放对象 |

直接注入验证覆盖 transport 和实际 GB sender policy。其他 session 的 overflow/error 分支通过源码检查和真实协议生命周期验证，不宣称每个协议均单独注入了相同错误。单次正常 RTCP 报告远小于 1 MiB，不为不可达的队满场景增加私有测试接口。

## 10. CTest

原 7 个测试全部保留，增加 UDP 后为 8/8。普通 Debug、性能 RelWithDebInfo、ASan/UBSan 合并构建及独立 UBSan 构建全量通过。日志分别为 `stage6-ctest.log`、`perf-ctest.log`、`asan-ctest.log` 和 `ubsan-ctest.log`；报告提交阶段再执行普通 build 与全量 CTest，记录 `final-build.log`、`final-ctest.log`。

## 11. ASan/UBSan

沿用已验证的 CMake 配置，未调整 sanitizer 选项：address 构建实际启用 `-O1 -g -fsanitize=address,undefined`，使用静态 Boost 1.92 ucontext/ASan；原 CMake 配置检查成功。独立 undefined 构建同样使用原配置。运行选项为 `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`、`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`。

两种构建都通过 CTest、真实协议 smoke、换源、churn、异常断开和 33 秒 GB RTCP。没有 ASan/LSan/UBSan 错误诊断；标准 makecontext/swapcontext 支持警告保留在日志中。没有关闭 fake stack 或 quarantine。协议套件使用 sanitizer server 和正常构建 clients；预编译第三方库并非全部插桩。

churn 普通构建 settled RSS 为 67,836 KiB，ASan 为 7,547,268 KiB，UBSan 为 195,412 KiB；三者 FD 均 26→26，threads 均 6→6。ASan RSS 受原有运行时配置影响，不用作普通性能比较或内存优化结论。

## 12. WHEP/WHIP/GB/RTSP UDP 回归

共 18 个成功的真实协议套件，完整命令、配置、结果摘要及原始结果 SHA256 在 JSON 中。普通 smoke 覆盖默认 RTSP TCP，并额外运行 RTSP UDP publish；ASan smoke 覆盖默认 RTSP TCP，ASan 换源及异常断开覆盖 RTSP UDP；UBSan smoke 直接覆盖 RTSP UDP。所有 smoke 对 RTMP、RTSP、WHIP 输入验证 1/4 viewers 的 RTMP、RTSP、WHEP、HTTP-FLV、HLS 输出，并验证 GB UDP、GB TCP active/passive 转发。RTMP/RTSP 输入另用 FFmpeg 独立双轨解码验证。

新增 `bench/lifecycle_verify.py --rtsp-publish-transport udp`，默认值仍为 tcp；只选择发布 transport。新增 `bench/udp_rtcp_verify.py` 通过独立 RTCP 转发 socket 观察 GB sender 的真实 SR 与 receiver RR。普通、ASan、UBSan 各观察 33 秒，均收到 1 个 SR、2 个 RR，保留原 25 秒 sender 周期；停止后的 DELETE 204→404。

部分 RTSP UDP 输入采样有内核 socket 接收 drops：普通 UDP smoke 最大 67，普通 UDP 换源最大 176，普通异常断开最大 10；ASan UDP 换源最大 216、异常断开最大 74；UBSan UDP 换源最大 60。这些是现存 socket 的 `/proc` 接收 drop 计数，不是本次 transport 写队列拒绝；不能将关闭 socket 后采样回到 0 解读为累计丢包为 0。对应媒体进度与失败断言全部通过，server queue full 为 0。没有匹配旧源码的 RTSP UDP 同条件测量，不将这些观测归因于本次重构，也不宣称所有协议测试零内核丢包。保留原接收路径和 socket buffer 策略。

## 13. source replacement

普通构建 RTMP、RTSP TCP、WHIP、GB UDP 各 20 代，共 80；另验证 RTSP UDP 20 代与 GB TCP active 20 代，共 120。ASan 和 UBSan 分别对 RTMP、RTSP UDP、WHIP、GB UDP 各 20 代，共 80。套件验证旧 viewer EOF、旧 HLS ENDLIST/404、新代 location 与媒体继续输出，全部通过。

普通、ASan、UBSan 另分别完成 RTMP/RTSP/HTTP-FLV/HLS/WHEP 每类 300 viewers、WHIP 300 次创建/媒体/删除、GB UDP 300 pairs churn，batch 20。每轮最后 300 个过期 HLS lease 返回原有 403，FD 和线程回到基线。

## 14. abnormal disconnect

普通、ASan、UBSan 各 12 cases：RTMP/RTSP UDP 发布控制连接 RST、RTMP/RTSP viewer RST、HTTP-FLV RST、RTMP 半连接、RTSP SETUP 无 PLAY、GB UDP peer 消失后显式删除与端口复用、WHEP/WHIP 已建立 peer 关闭与 DTLS 超时。验证既有关闭时序、一次 shutdown、404、端口释放和后续恢复媒体，全部通过。settled FD 为 27，与仍运行一个 publisher 的 active-idle 一致；threads 为 6。

## 15. WHEP 100/500/1000 并发结果

同机、相同 fixture，6 workers、8 client IO threads、100 viewers/s ramp；各级 warmup 5 秒、measurement 20 秒，依次执行。原 2 GiB RSS guard 和 5.4 CPU cores stop 未触发；达到计划 1000 viewers 后结束，不声称探测最大容量。

| Viewers | CPU cores | RSS median KiB | RSS peak KiB | 吞吐 Gbit/s | 双轨首媒体 p50/p95/p99 ms | Failures / drops / queue full |
| --- | ---: | ---: | ---: | ---: | --- | --- |
| 100 | 0.3670 | 58,802 | 61,612 | 0.8011 | 505 / 955 / 994 | 0 / 0 / 0 |
| 500 | 1.7971 | 189,342 | 200,292 | 4.0124 | 510 / 964 / 1003 | 0 / 0 / 0 |
| 1000 | 3.8184 | 373,044 | 403,644 | 8.0146 | 511 / 974 / 1015 | 0 / 0 / 0 |

每级 requested/ready/media_ready/progressing/stopped/removed 均等于对应 viewers 数，SRTP unprotect failures 为 0。全部结果见 `capacity-summary.json` 和各级原始目录。

## 16. 与 c25092b 性能基线比较

采用 c25092b 记录的三轮 `whep-1000` workload，warmup 10 秒、measurement 30 秒、runs 3，未调整 benchmark。基线 JSON 的测量源码为 `8af9884`，其后 c25092b 只记录验证文档。同一 i7-13700KF/31 GiB 主机、Linux 6.8.0-136、GCC 16.0.1、Boost 1.92、jemalloc 5.3、OpenSSL 3.0.2、libSRTP 2.7、服务 FFmpeg 9.0.1 libraries、测试 FFmpeg CLI 7.1.1。RelWithDebInfo 为 `-O2 -g -DNDEBUG`，workers/IO/ramp 一致。三个原生 client binary SHA256 与基线完全相同。

fixture 复用原文件，SHA256 为 `446b462c4b408d997c17cd4bab82d3ca4e9bcda1f04c758471043518d62b800f`，H264 720p30/AAC 44.1k stereo。性能测量期间其他构建与协议压力工作已结束。

| 三轮中位数 | c25092b 记录的基线 | 本轮 | 变化 |
| --- | ---: | ---: | ---: |
| CPU cores | 3.760166 | 3.847461 | +2.3216% |
| RSS KiB | 379,504 | 372,968 | -1.7222% |
| 吞吐 Gbit/s | 8.027526 | 8.032323 | +0.0598% |
| 双轨首媒体 p50 ms | 502 | 501 | -1 ms |
| Failures / drops / queue full | 0 / 0 / 0 | 0 / 0 / 0 | 均为 0 |

三轮 CPU 分别为 3.778923、3.847461、3.877841 cores；RSS 分别为 372,968、371,022、380,640 KiB；双轨首媒体 p50 为 501、509、496 ms。CPU 有小幅上升，RSS 略降，吞吐和延迟基本持平；本 workload 没有观察到不可接受退化，不将短测量推导为所有负载或长期性能保证。5/20 秒容量结果与此 10/30 秒基线分开报告。

## 17. 是否发现生产 bug

未复现本轮新增生产回归。旧 GB sender 的 yield 发送引用 session queue buffer，而 shutdown 可清 queue，存在 buffer lifetime 契约缺口；本次用 handler 独立共享持有收口，未声称在旧源码 ASan 下复现 UAF。

迁移中发现用 `no_buffer_space` 同时报 overflow 和真实发送错误会混淆 session policy，已在 f572de3 修正，并重新验证全部回归。RTSP UDP 输入接收 drops 如第 12 项记录，未作新旧归因或调整产品接收策略。

早期编译的 Wshadow、测试初始 fixture 选择、Python 3.10 测试工具接口错误均已修正，失败日志保留；最终验收只引用修正后成功结果，不把这些失败隐藏或解释为生产测试已通过。

## 18. git status

完成交付时工作树干净，HEAD 等于 fetch 后 origin/main。本报告 commit/push 后执行最终核验，完整 SHA、提交列表、空 status 与分支一致性记录在 `/tmp/media_server_udp-c25092b/final-git.json`。不在提交内部嵌入其自身未知 hash。

## 19. 是否全部 push

六个代码/验证提交逐阶段 push；本报告作为最后一个独立提交 push。最终 fetch/rev-parse 核验确认远端包含全部提交。原始证据目录中的 `progress.json` 随最终交付标记完成；没有剩余任务拥有的协议 server 进程。

## 20. 是否还有 session 自己维护的通用 UDP 写队列

没有。生产代码 `async_send_to` 只有 `media/net/udp_transport.cpp` 一处。`queued_write_bytes_` 只存在于 UDP/TCP transport。旧 transport 文件/类型已删除，没有 alias、旧 wrapper 或兼容 header；`udp_write_queue_`、`run_udp_write` 在生产 session 中无匹配。

剩余 TCP transport queue、HTTP/Beast 写路径和 benchmark 的独立 UDP client 不属于 UDP session scheduling。报告与 JSON 中的旧符号只用于说明删除项和保存搜索命令，不属于实现残留。
