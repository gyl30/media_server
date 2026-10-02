# 跨工作线程 Sink 分发统一验证

## 基线与范围

BASE_HEAD：`1f9d4f3c46266c401ebab62b5f69a4a39cdacee9`。

开始时已执行 `git fetch origin`、完整工作区检查、HEAD/远端比较及最近 15 条提交检查，工作区干净且 HEAD 等于 origin/main。最终文档提交的 HEAD 可通过 `git log -- docs/worker_sink_dispatcher_verification.md` 查询，避免在文档中写入自身尚未生成的提交号。

生产实现提交：`68c45e0`、`80b5295`。测试提交：`9abc847`、`d2f91f6`。后者仅增强真实协议测试的异步关闭屏障，不改变 GB 产品时序。

本轮只提取执行层，不改变媒体类型、协议、packetizer、transport、registry、worker_context、媒体端口或超时策略；不增加缓存、reader/cursor、历史重放或配置层。

原始日志和命令位于 `/tmp/media_server_fanout-1f9d4f3`。结构化验收摘要见 `docs/verification_results/worker_sink_dispatcher.json`。

## 唯一实现与职责

原来 `media_stream` 和 `mpeg_ps_output` 各自实现一份 sink_group：目标 worker、组内 mutex、弱 sink 表、pending deque、drain_queued、pending_end、snapshot、drain、overflow detach 及 end 调度。现在这些细节仅存在于 `media/core/worker_sink_dispatcher.h`。

组件使用编译期模板参数 `Frame`、`Sink` 和帧交付成员函数指针。两种业务分别绑定 `media_sink::on_frame` 和 `mpeg_ps_sink::on_ps_frame`，没有新增逐帧 std::function、type erasure、generic virtual base 或 policy class。

API 仅包含 owner 构造和四个操作：

```cpp
worker_sink_dispatcher(worker_context& owner);
add(std::shared_ptr<Sink> sink, worker_context& target);
remove(Sink* sink);
publish(const Frame& frame);
end();
```

`target` 由调用侧在原来的 caller worker 上取得，保留旧版 `sink->worker()` 调用时序。group collection 的变更依赖已有 owner-worker 串行化；没有引入 collection mutex。业务类仍保留公开 add/remove 中捕获 shared self 的 dispatch 和终态检查，dispatcher 接收已经串行化的调用。

## 保持的并发契约

| 项目 | 当前实现及保留的行为 |
| --- | --- |
| same worker | publish 直接交付，无 pending 入队、无 post。普通 end 直接调用 on_end。结束后新注册 sink 的 on_end 仍由业务类 post，保留旧时序。 |
| cross worker | 按 target worker 分组，同组多个 sink 共用 pending 和一次 drain 调度。drain_queued 保持一个已排队或正在执行的 drain，不按 viewer 增加 post。 |
| 帧 ownership | 每个跨 worker group 存一份 Frame；payload 的 shared ownership 保持原语义，不复制媒体字节。已提交的 handler 只持有 shared group，不要求业务 source 持续存活。 |
| high-water | 固定 2500 个 pending frame。已经 swap 到 target 本轮 drain 的 batch 不计入 pending，与旧算法相同。 |
| overflow | 第 2501 个未 drain 的 frame 到来时清 pending、标记 pending_end、必要时调度一次 drain，并按 group identity 从 owner collection detach。结束该慢 worker group，其他 group 继续。已经进入 target 当前 batch 的帧仍按旧时序处理。 |
| end ordering | 普通 end 清 owner collection、标记跨 worker pending_end；已排队帧全部交付后再 on_end。overflow 的清 pending 是既有独立策略。 |
| weak lifecycle | 注册表只持 weak_ptr。snapshot 清理 expired 项，并为当前 batch 短暂提升 shared ownership。注册本身不延长 sink 生命周期。 |
| remove/snapshot | remove 清注册，不撤销 target 已取得的 snapshot；旧 snapshot 仍可继续其 batch 回调。保留调用侧 terminal fencing 所依赖的窗口，没有新增“remove 后绝无回调”保证。 |
| 终态 | 业务 owner 仍管理 ended_ 或 source_/muxer_，阻止终态后的新注册。dispatcher end 只结束并清空现有 groups；后续 publish 无 group 可投递，不接管 PS muxer 生命周期。 |

`media_stream` 保留 tracks、ended_、空 payload/未知 track 检查、派生 PS ownership 和公开 API。`mpeg_ps_output` 保留 PS muxing、track mapping、waiting_for_key_frame_、source_、muxer_、packet_。其 finish 顺序仍为 detach source、清 source、结束 sinks、释放 packet、释放 muxer，只用 dispatcher.end 替换结束 sinks 的重复实现。

仓库的 `mpeg_ps_sink` 实际定义在 `media/ps/mpeg_ps_output.h`，未新增不存在的独立兼容头。

## 代码量

| 生产文件 | 基线行数 | 当前行数 | 变化 |
| --- | ---: | ---: | ---: |
| media_stream.h | 50 | 46 | -4 |
| media_stream.cpp | 312 | 112 | -200 |
| mpeg_ps_output.h | 74 | 71 | -3 |
| mpeg_ps_output.cpp | 400 | 194 | -206 |
| worker_sink_dispatcher.h | 0 | 240 | +240 |
| 合计 | 836 | 663 | **-173** |

两个业务 cpp 共减少 406 行。上述是生产实现净变化；另增加 395 行针对性测试、4 行 CMake 注册和 9 行测试屏障，因此包含新增测试的全仓差异净增加 235 行。没有用删除验证代码来满足代码量指标。

## 测试先行

新增 `tests/worker_sink_fanout.cpp`，通过真实 media_stream 与 MPEG-PS 公共边界分别执行 A–H，未暴露 private 状态或使用 mock dispatcher：

- same-worker 帧交付在 publish 返回前完成；帧顺序、无效媒体输入、重复 end、终态 publish 和 late-add END 时序。
- 跨 worker 1/2/3/END 顺序；同 target 三个 sink 和多个独立 target；同一 queued batch 仅一个 target handler；共享 payload identity。
- source/PS 业务对象销毁后，已排队 group 仍能交付 pending 和 END。
- weak sink 过期、pending remove、真正双线程下 snapshot 后 remove：无悬空访问，保留旧 snapshot 回调窗口，无重复 END。
- 精确 2500 边界全部交付；超过边界的慢 group 只结束，快 group 不受影响，慢 group detach 后不再收到帧或第二次 END。

基线生产实现先通过新增测试。然后临时植入三种错误，验证测试确实变 RED：same-worker 改为 post、threshold 改为 2499、普通 end 清 pending。三个错误分别被 inline、边界、ordering 断言捕获。逐字恢复原生产文件后完整 build/CTest 再 GREEN；这些临时 mutation 是测试有效性验证，不是发现的生产 bug，也未提交。

每个实现阶段均执行 diff check、`cmake --build build -j12`、完整 `ctest --test-dir build --output-on-failure` 后单独提交并 push。CTest 从原 8 项增至 **9/9**；原有 media_lifecycle 和 UDP transport 测试均保留。

## 构建与 sanitizer

普通 Debug、性能 RelWithDebInfo、ASan+UBSan 和独立 UBSan 均构建成功，完整 CTest 各 **9/9 PASS**。

复用仓库已验证配置，没有调整 sanitizer 开关：ASan 使用静态 Boost 1.92 ucontext sanitizer 构建，CMake compatibility guard 成功；实际目标 flags 为 `-O1 -g -fsanitize=address,undefined`。独立 UBSan 为 `-O1 -g -fsanitize=undefined`。运行选项为 `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` 和 `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`。保留标准 ucontext 警告，不关闭 fake stack/quarantine。第三方预编译库插桩范围与原基线一致。

性能二进制使用 GCC 16.0.1、RelWithDebInfo `-O2 -g -DNDEBUG`、jemalloc；San 协议服务器使用对应插桩二进制，客户端使用普通性能构建。

## 测试工具失败与修正

首次 ASan smoke 在 GB UDP pair 删除后，立即创建同名 GB TCP active receiver 时返回 HTTP 500。证据完整保存在 `asan-smoke-before-barrier.log` 和同名子目录。日志没有 sanitizer 错误，失败发生于 source-exists 检查，尚未进入 TCP listener startup。

既有 GB DELETE 在 session 登记移除后立即返回；source 清理由 receiver owner 异步完成。原测试将 DELETE/重复 DELETE 的状态当成 source 已同步删除，存在竞态。`d2f91f6` 增加与既有 publisher 同类的关闭屏障：旧 source 的 HTTP-FLV 路径必须返回 404，才继续创建下一 pair。没有 retry create 吞掉 500，没有修改产品超时、GB state machine、关闭顺序或 registry。

修正后重新执行完整 ASan smoke，已 PASS；不是跳过失败场景。其余完整回归结果在下文记录。

## 真实协议回归

普通构建 8 套、ASan 5 套、UBSan 5 套，共 **18/18 PASS**。复核各 result.json 的失败字段、命令及所有子日志，无非零 failed/runtime_failures/unprotect_failures，无 ASan/LSan/UBSan 错误，无 write queue full。

| 场景 | 普通 | ASan+UBSan | 独立 UBSan |
| --- | --- | --- | --- |
| 多协议 smoke | PASS | PASS | PASS |
| RTMP / RTSP / WHIP / GB UDP 换源 | 各 20 代 PASS | 各 20 代 PASS | 各 20 代 PASS |
| 额外 RTSP UDP 换源 | 20 代 PASS | 上一行使用 UDP | 上一行使用 UDP |
| GB TCP active / passive 换源 | 各 20 代 PASS | smoke 覆盖两模式 | smoke 覆盖两模式 |
| session churn | 每协议 300 次 PASS | 每协议 300 次 PASS | 每协议 300 次 PASS |
| abnormal disconnect | 12 个场景 PASS | 12 个场景 PASS | 12 个场景 PASS |
| GB UDP RTCP | 33 秒 PASS | 33 秒 PASS | 33 秒 PASS |

smoke 覆盖 RTMP、RTSP TCP、WHIP source 到 RTMP、RTSP、HTTP-FLV、HLS、WHEP 的单 viewer/多个 viewer，以及每种 source 经 PS 输出、GB UDP/TCP active/TCP passive sender/receiver relay 的真实链路。RTMP、RTSP source 的 RTMP/RTSP/HTTP-FLV/HLS 输出另经独立 FFmpeg 视频和音频解码检查。WHIP 则复用既有真实发布器和各协议接收端的媒体推进、SRTP 校验断言，不将其描述为独立 FFmpeg 解码覆盖。

换源共 300 个 generation；检查旧 HTTP-FLV sink 结束、HLS ENDLIST/失效语义及 generation 的 session 地址不复用。GB 的 UDP 和两种 TCP 换源同时经过 canonical dispatcher 和派生 PS dispatcher。针对性测试另覆盖业务对象已销毁但 group 仍有 pending 的存活/结束行为。

churn 每套的 RTMP/RTSP/WHEP/HLS/HTTP-FLV viewer、WHIP publisher、GB pair 各 300 次；三种构建均从 FD 26 / 线程 6 回落到 FD 26 / 线程 6，300 个 HLS lease 均返回 403。异常断开每套包括 RTMP/RTSP publisher reset、真实播放后的 RTMP/RTSP viewer reset、GB UDP peer 消失及端口释放、HTTP/RTMP 半连接、RTSP SETUP 无 PLAY、WHEP/WHIP 建立后消失和 DTLS 超时，随后媒体恢复；活动空闲 FD 27 / 线程 6 与最终状态一致。

GB UDP RTCP 保持 25 秒产品间隔，观察 33 秒；三种构建各收到 1 个 SR、2 个 RR，媒体持续推进。没有改变 RTP packetization、RTCP timer 或协议状态。

未将协议丢包数据隐藏为零：普通套件全部采样为 0；ASan RTSP UDP 换源的 `udp_drops` 最高样本 198，异常断开恢复采样 77；UBSan RTSP UDP 换源最高样本 30。它们是当时服务器 socket 的内核接收计数采样，不是可直接相加的全程总丢包，也不是 dispatcher overflow 计数；对应媒体推进和生命周期断言仍通过。WHEP 容量的 drop/queue 指标单独记录。

## 性能条件

同机 Ubuntu 22.04 / Linux 6.8.0-136-generic，i7-13700KF，16 核 24 逻辑线程、约 31 GiB 内存。服务使用 GCC 16.0.1、Boost 1.92、OpenSSL 3.0.2、SRTP 2.7、FFmpeg 9.0.1 库、jemalloc 5.3；测试 CLI 为 `/home/gyl/bin/ffmpeg` 7.1.1。

复用原 fixture `/tmp/media_server_verify-728178f/h264-aac.flv`，SHA256 `446b462c4b408d997c17cd4bab82d3ca4e9bcda1f04c758471043518d62b800f`；720p30 H.264 + 44.1 kHz stereo AAC，10.023 秒，9,796,818 bytes。每级容量为 1 个 RTMP source、6 个 server worker、8 个 client I/O 线程、100 viewers/s ramp、5 秒 warmup、20 秒 measurement。所有协议/San 负载退出后，容量档位依次执行，没有与本轮构建或协议套件并行。首轮与同机其他项目的编译/测试重叠，不能作为无竞争性能对照，相关失败和复测另行记录。

为判断是否存在本轮回归，重构前已使用原性能 server 二进制完成同条件 WHEP 1000 对照。该二进制 SHA256 为 `c98999c0241ef590d9632bfe1b85952613d0e84e5431482b8fa33d46ed2394ac`，与 UDP 稳定基线一致；测试提交 `9abc847` 不含生产修改。对照 CPU 4.05707 cores、RSS 中位数 392,364 KiB、吞吐 8.00274 Gbit/s、双轨 first-media p50 495 ms，失败/drop/queue full 均为 0。

此处使用同窗口、同 5/20 秒 workload 的重构前后对照。`c25092b` 的三轮性能基线包含不同 warmup/measurement 条件，不用一次短容量运行去计算对其的性能改善百分比。

## WHEP 容量及性能异常定位

首轮 100 PASS；500 虽无 drop/queue full，但 CPU 异常升至 3.745 cores；1000 返回失败，CPU 5.8746 cores、RSS 中位数 1,285,972 KiB、吞吐 7.8945 Gbit/s、2,564,362 次 write queue full。所有 viewer 建立和推进并不等于容量 PASS，未忽略服务器 queue full。失败原始结果/日志保留在 `capacity-1000`，server.log 约 253 MiB。

当时只读进程检查发现同机其他项目正在编译/CTest；未停止或修改它们。按环境竞争、算法增加工作、构建/采样差异的顺序调查。二进制哈希、实际编译 flags、fixture、参数均已核对；针对性测试和静态逐段对照确认 same-worker inline、每 group 一次 post、无新增运行时 callback、无新增媒体字节复制。

随后在未检测到编译/CTest 进程时，紧邻串行运行原基线 1000、新二进制 500/1000，**未修改任何生产代码、构建配置或 benchmark/workload**。旧、新均恢复且通过；这支持瞬时同机竞争的解释，不将某个外部进程的贡献宣称为已单独证明。旧二进制复测结果为 CPU 3.85746 cores、RSS 372,546 KiB、吞吐 8.01722 Gbit/s、first-media p50 497 ms、queue full/drop 0。

最终接受结果如下，100 使用首轮通过结果，500/1000 使用明确标识的复测目录；失败目录未覆盖：

| Viewers | CPU cores | RSS 中位数 KiB | RSS 峰值 KiB | Gbit/s | 双轨 first-media p50 ms | failures / drops / queue full |
| ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 100 | 0.3840 | 58,274 | 59,404 | 0.8011 | 500 | 0 / 0 / 0 |
| 500 | 1.8332 | 193,648 | 222,120 | 4.0124 | 511 | 0 / 0 / 0 |
| 1000 | 3.8268 | 374,966 | 429,252 | 8.0259 | 511 | 0 / 0 / 0 |

各档 ready、双轨 media_ready、progressing、stopped、removed 均等于请求数，SRTP unprotect failures 为 0，6 个 worker；RSS 2 GiB guard 和 5.4 cores CPU 停止阈值未触发。命令及结果分别见 `capacity-100`、`diagnostic-candidate-500`、`diagnostic-candidate-1000`，旧基线见 `diagnostic-baseline-1000`。

紧邻旧/新 1000 对照：CPU **-0.79%**、RSS **+0.65%**、吞吐 **+0.11%**、first-media p50 **+14 ms**。这是一组容量采样，不能当成完整三轮性能基线或统计显著性证明；未观察到持续 CPU/RSS/吞吐退化，first-media 仍接近原自然关键帧等待分布。不根据首轮竞争条件的失败去改变产品行为，也不把复测包装为性能优化。

## 最终结构与边界检查

`struct .*sink_group`、`drain_queued`、`pending_end`、`max_pending_frames`、`snapshot_sinks` 在 media/core 与 media/ps 的搜索结果全部只指向 worker_sink_dispatcher.h。生产 diff 仅涉及该新头、media_stream.h/.cpp、mpeg_ps_output.h/.cpp 五个文件。没有协议 session、TCP/UDP transport、packetizer 或 registry 修改。

未发现必须修复的生产回归。独立测试工具修正和容量失败均保留证据；没有将未通过的首次运行记为 PASS。后续 WebRTC transport 只读审计在本轮 fanout 验证文档 commit/push 后进行，另行记录，不实现新的 transport。
