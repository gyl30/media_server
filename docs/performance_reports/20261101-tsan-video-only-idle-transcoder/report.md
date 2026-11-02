# TSan 并发证书、纯视频 WHEP 与空闲转码器内存（2026-11-01）

基线 `main@fc1bcc1`。本轮只做定向实验，没有合入生产改动。

| 构建 | 内容 | Release `media_server` sha256 前缀 |
| --- | --- | --- |
| A | HEAD | `f24e6ca2bf0a4fd9` |
| B | HEAD + [`e-free-idle-transcoder.diff`](e-free-idle-transcoder.diff) | `153cd36e06b96a62` |
| C | HEAD + [`c-video-only-companion.diff`](c-video-only-companion.diff) | `f808275a60cac4d3` |

- 编译：GCC 16，RelWithDebInfo 加 hardening；sanitizer 构建为 Debug 的 address、thread。
- 测试机：与上一份报告相同，8 核服务端（CPU 0–7），客户端在 8–15、24–31，独立网络命名空间 198.19.0.10/11。
- 作业脚本在 [`jobs/`](jobs/)，原始结果只在测试机的 `runs5/`。

## 结论

| 项 | 分类 | 结论 |
| --- | --- | --- |
| OpenSSL `ossl_ht_get`/`ossl_ht_insert` 数据竞争 | TOOLING | OpenSSL 3.5.9 的 namemap 哈希表用 release/acquire 原子操作做无锁读，项目的 OpenSSL 静态库没有经过 TSan 插桩，TSan 看不到这对同步。换成插桩的 OpenSSL 后，同样的并发报告为 0 |
| 报告之后的 SEGV | TOOLING | TSan 在 128 KiB、用 `malloc` 分配、没有保护页的协程栈上做符号化，栈溢出，写坏了相邻协程栈顶的 fiber 记录。关闭符号化、加保护页、或把栈加大到 1 MiB，都能让 SEGV 消失；Release 与 ASan 下没有出现 |
| 纯视频 WHEP 触发 Opus→AAC | CONFIRMED | 纯视频 offer 的会话挂在主流上，使接收端一直执行无人消费的 Opus→AAC：64 源时服务端 CPU 是直通观众的 2.7 倍。改挂 Opus 伴随流的改动（+5/−1 行）使纯视频观众 CPU 下降 66–73%，视频输出、AAC 观众按需转码、源停止清理与 sanitizer 均无变化 |
| 空闲转码器内存 | 仅有潜在价值 | 每个空闲转码器在服务端约占 0.37 MiB PSS，1056 源时约 390 MiB（−33%）；释放后加入成本不变，因为现有实现每次加入本来就会重新初始化。内存不是 8 核容量的瓶颈，本轮不实施 |

值得实施的最小生产改动只有一项：[`c-video-only-companion.diff`](c-video-only-companion.diff)，纯视频 WHEP 改挂 Opus 伴随流。本轮没有合入，待授权。

除此之外，没有找到有证据支持的媒体核心改动，媒体核心精简专项到此结束。

测试工具改动：`bench/whep_fanout.cpp` 的 `--video-only` 改为在 offer 中去掉音频 m-line（原来只放宽音频检查，没有脚本使用），未提交。

## 1. TSan：并发生成 DTLS 证书

### 1.1 环境

- 项目依赖由 `deps/build.sh` 构建：OpenSSL 3.5.9 `no-shared no-tests no-docs -fPIC`，没有 `-fsanitize=thread`；Boost.Context 另有 `context-impl=ucontext`、`BOOST_USE_TSAN` 的 TSan 版本。TSan 构建用 GCC 16、`-O2 -g -fsanitize=thread`、静态 TSan 运行时。
- `whip::create()`、`whep::create()`、`whep_pull_session` 每个会话都调用 `dtls_certificate::create()`（EC P-256 密钥、自签 X509、SHA-256 指纹），运行在 `worker_context::spawn` 启动的协程里。协程栈是 Asio 默认的 `boost::context::fixedsize_stack`：128 KiB，`std::malloc` 分配，没有保护页。

### 1.2 两份报告分开看

真实浏览器测试（上一份报告 p02、p05）与本机复现中，失败日志都包含两段：

1. `WARNING: ThreadSanitizer: data race`：读方 `memcmp ← ossl_ht_get`，写方 `memcpy ← ossl_ht_insert`，写方持有两把由 `CRYPTO_THREAD_run_once` 创建的 rwlock，读方不持有锁；堆块 104 字节，由 `CRYPTO_malloc` 分配；两个"线程"都是协程 fiber（`tid=0`，由 `create_fiber1` 创建）。
2. `ThreadSanitizer: SEGV`：在主线程 worker 的 `spawned_thread_resumer::operator()()`（`boost/asio/impl/spawn.hpp:215`）读 `spawned_thread_->attach()` 时访问非法地址，触发点是 accept 协程的 `reactive_socket_accept_op` 完成（新连接或退出时取消）。

### 1.3 数据竞争

OpenSSL 3.5.9 源码：

- `crypto/core_namemap.c:524`：namemap 的哈希表配置为 `collision_check=1, lockless_reads=1`；查名 `ossl_namemap_name2num` 直接调用 `ossl_ht_get`，不加锁。
- `crypto/hashtable/hashtable.c:644`：插入时先 `memcpy` key 到新值的 `keybuf`，再在 `ossl_ht_insert_locked` 中用 `ossl_rcu_assign_ptr` 发布；`crypto/threads_pthread.c:532` 中它是 `__atomic_store(..., __ATOMIC_RELEASE)`。
- 读方 `ossl_rcu_deref` 是 `__atomic_load_n(..., __ATOMIC_ACQUIRE)`（`threads_pthread.c:527`），之后才 `memcmp` 比较 key（`hashtable.c:549`）。

这是正确的 release/acquire 发布。OpenSSL 没有插桩时，TSan 只拦截到 `memcpy`/`memcmp`，看不到中间的原子操作，于是报告竞争。namemap 只在首次查到某个算法名时插入，所以只有进程刚启动、多个线程同时首次生成证书时才会出现。

最小复现 [`cert_stress.cpp`](cert_stress.cpp) 直接编译项目的 `media/webrtc/dtls_certificate.cpp`，与服务端使用相同的 GCC 16、TSan 运行时、OpenSSL 静态库和 TSan 版 Boost.Context：

```
g++-16 -std=c++23 -O2 -g -fsanitize=thread -Wno-tsan -U_FORTIFY_SOURCE -DBOOST_USE_UCONTEXT -DBOOST_USE_TSAN \
  -I. -Ideps/install/include cert_stress.cpp media/webrtc/dtls_certificate.cpp -o cert_stress \
  deps/install/tsan/lib/libboost_context.a deps/install/lib/libcrypto.a -static-libtsan -lpthread -ldl
```

插桩对照只换 OpenSSL：同一份 3.5.9 源码加 `CC=gcc-16 -fsanitize=thread -g` 编译（`objdump` 确认 `ossl_ht_get` 内有 13 处 `__tsan_*` 调用）。

| 模式（各 40 次进程） | 未插桩 OpenSSL：`ossl_ht` 报告 | SEGV | 插桩 OpenSSL：报告 | SEGV |
| --- | --- | --- | --- | --- |
| 串行 8 次 | 0 | 0 | — | — |
| 2 个 `std::thread` 同时 | 8 | 0 | 0 | 0 |
| 8 个 `std::thread` 同时 | 24 | 0 | 0 | 0 |
| 32 个 `std::thread` 同时 | — | — | 0 | 0 |
| 8 个协程，1 个 worker | 0 | 0 | — | — |
| 8 个协程，2 个 worker | 23 | 0 | 0 | 0 |
| 8 个协程，4 个 worker | 14 | 0 | 0 | 0 |

- 报告需要至少两个 OS 线程同时首次生成证书，与是否使用协程无关；单 worker 下协程串行执行，从不出现。
- 插桩后任何模式都没有报告，也没有出现其他 TSan 报告。

### 1.4 SEGV

[`whip_burst.py`](whip_burst.py) 在本机启动 `--threads 2` 的服务端和签名桩，同时 POST 3 个 WHIP offer，1 s 后 SIGTERM。TSan 构建（`build-san-thread`，与测试机同一构建配置）：

| 运行条件（各 40 次） | 竞争报告 | SEGV / 崩溃 |
| --- | --- | --- |
| 默认 | 6 | 6，每次都跟在报告之后，在 accept 协程恢复处 |
| `TSAN_OPTIONS=symbolize=0` | 4（未符号化，正常输出） | 0 |
| `TSAN_OPTIONS=report_bugs=0` | 0 | 0 |
| 实验构建：`protected_fixedsize_stack(128 KiB)`（[`t-guarded-fiber-stack.diff`](t-guarded-fiber-stack.diff)） | 0 次完整输出 | 3：2 次进程直接被 SIGSEGV 终止（无输出），1 次 `DEADLYSIGNAL` + `nested bug in the same thread` |
| 实验构建：`protected_fixedsize_stack(1 MiB)` | 3（60 次中，完整输出） | 0 |

- 报告是在发现竞争的 fiber 上生成的，符号化（libbacktrace 加 C++ 反修饰，服务端报告共 175 个栈帧，最长的单帧近 2000 字符）在 128 KiB 栈上溢出。
- 默认栈由 `malloc` 分配，相邻的通常是另一个协程的 128 KiB 栈；Boost.Context 把 fiber 记录和 `spawned_fiber_thread` 放在栈顶。溢出写坏了相邻协程（这里是 accept 协程）的记录，它下次恢复时读到非法的 `spawned_thread_` 指针而 SEGV。
- 加保护页后，溢出变成报告过程中的立即崩溃；栈加大到 1 MiB 后，报告完整输出，没有崩溃；关闭符号化后，同样有报告却没有崩溃。三者共同证明 SEGV 是 TSan 报告路径的栈溢出，不是业务代码的内存损坏。

独立压力程序中的协程栈帧很浅，报告也不会溢出，所以那里只有竞争、没有 SEGV。

### 1.5 Release 与 ASan

同一个 `whip_burst.py`，每次 8 路并发 WHIP：

| 构建 | 次数 | 全部 201 | 崩溃 | Sanitizer 报告 |
| --- | --- | --- | --- | --- |
| Release（`build-rel`） | 30 | 30 | 0 | — |
| ASan+UBSan+LSan（`build-san-address`） | 30 | 29 | 0 | 0 |

ASan 那次失败是随机选端口冲突（`http listen failed ... Address already in use`），服务端没有启动，与本问题无关。上一份报告中 Release 以每秒 200 路并发建立 1056 个 WHIP 也没有崩溃。

### 1.6 处理建议

- 生产代码不需要修改：没有加全局锁，没有共享证书，也没有关闭任何检查。
- 不是 OpenSSL 缺陷，也不是 Boost.Context 缺陷，没有可提交的上游问题。
- 如果要让 TSan 浏览器测试稳定，可以在 TSan 构建中改用插桩的 OpenSSL（`deps/build.sh` 像 Boost.Context 一样多编一份 `tsan/` 前缀），这样竞争报告就不会出现，SEGV 也不会被触发。这会让依赖构建多出约数分钟，属于测试工具改动，本轮没有实施。
- 在那之前，TSan 浏览器结果中如果只出现 `ossl_ht_get`/`ossl_ht_insert` 竞争以及随后的 `spawn.hpp:215` SEGV，就按本节归为工具问题；其他 TSan 报告仍需单独分析。

## 2. 纯视频 WHEP

### 2.1 代码路径

- `whep::create()`（`media/webrtc/whep.cpp:200`）先用主流（AAC）轨道生成 answer。纯视频 offer，或音频 m-line 被拒绝时，answer 中没有音频 payload type，`audio_codec` 为空，不满足改挂 Opus 伴随流的条件 `answer->audio_codec == codec_id::aac`，会话因此挂在主流上。
- `whep_session::startup()` 结束时调用 `stream_->add_sink(self)`，主流 `has_sinks()` 为真；WHIP / WHEP Pull 的接收端因此持续执行 Opus→AAC，而这个观众不接收任何音频。

### 2.2 测量

工具：[`viewer_cost.py`](viewer_cost.py) 在一个服务端上跑 N 个 WHIP 源（`whip_publisher`，同一素材），依次执行观众步骤。每步：
- 用 `--start-file` 对齐客户端测量窗口和服务端 30 s 的 `perf stat`，同时记录 PSS；
- 用 [`opus_aac.bt`](opus_aac.bt) 统计 `audio_transcoder::startup` 中 Opus→AAC 的启动次数，以及 `audio_transcoder` 的析构次数。

纯视频 offer 由 `whep_fanout --video-only` 发出：本轮对测试客户端做了改动，`--video-only` 时从 offer 中去掉音频 m-line，并把 BUNDLE 改为只含 `0`（`bench/whep_fanout.cpp`）。此前没有任何脚本使用这个选项。

8 核服务端，每源 1 个观众，两轮（A = HEAD 构建，`q02`、`q05`）：

| 步骤 | 64 源 Gcycles/s | 64 源 Opus→AAC 启动 | 256 源 Gcycles/s | 256 源 Opus→AAC 启动 |
| --- | --- | --- | --- | --- |
| 无观众 | 0.42–0.45 | 0 | 1.36–1.41 | 0 |
| 纯视频 WHEP | 2.39–2.42 | 64 | 9.43–9.59 | 256 |
| Opus 直通 WHEP（`stereo=1` offer，收音频） | 0.86–0.91 | 0 | 2.65–2.88 | 0 |
| 纯视频 WHEP + 1 个 FLV | 2.72–2.76 | 64 | 10.5–11.0 | 256 |

- 纯视频观众收到的音频包为 0，但每源比直通观众多约 24 Mcycles/s（64 源：(2.40 − 0.89) / 64），正好是一路 Opus→AAC 的成本。
- 换算到 8 核容量：同样负载下，纯视频 WHEP 每源的服务端开销是直通的约 2.7 倍。

### 2.3 候选改动

[`c-video-only-companion.diff`](c-video-only-companion.diff)：answer 没有音频 payload type、且源有 Opus 伴随流时，直接把会话挂到伴随流上。answer 保持不变。

```cpp
if (const auto& opus = stream->opus_stream(); opus && !answer->audio_payload_type)
{
    source = opus;
}
else if (opus && answer->audio_codec == codec_id::aac)
```

为什么视频协商和生命周期不变：
- 伴随流与主流使用同一条视频轨（`whip_media_receiver.cpp:233-246`，id 相同）；视频帧先发布到伴随流（`:279`）；源析构时两条流都 `end()`（`:194-201`）。
- `webrtc_packetizer::startup()` 对 payload type 为 −1 的音频轨直接跳过。
- 没有新增状态；纯视频观众不再算作主流的 sink。

A/C 对照（`q05`，两轮，C 为 HEAD 加上述补丁）：

| 步骤 | 64 源 A | 64 源 C | 256 源 A | 256 源 C |
| --- | --- | --- | --- | --- |
| 纯视频 WHEP，Gcycles/s | 2.39–2.41 | 0.77–0.81 | 9.55–9.59 | 2.62 |
| 纯视频 WHEP，Opus→AAC 启动 | 64 | 0 | 256 | 0 |
| 直通 WHEP，Gcycles/s | 0.86–0.89 | 0.87–0.92 | 2.87–2.88 | 2.85–2.87 |
| 纯视频 + FLV，Gcycles/s | 2.72–2.73 | 2.71–2.78 | 11.0 | 10.97–10.98 |
| 纯视频 + FLV，Opus→AAC 启动 | 64 | 64 | 256 | 256 |

- 纯视频观众的服务端 CPU 下降 66–73%，与直通观众相当；视频包数相同（64 源 873 k 对 873 k），客户端 RTP 检查 0 运行时失败。
- FLV 加入时仍按需启动 Opus→AAC；直通、FLV 的结果与 A 相同。
- 源停止（`video_stop`：观众在线时停止推流端）：A 与 C 都是每个 WHEP 会话走"源结束"路径（`webrtc source stream ended` 与 `whep session shutdown` 数都等于观众数），receivers 清零。测试客户端 `whep_fanout` 不会因服务端关闭而提前退出，要等到时长结束，A、C 相同。
- C 的 ASan+UBSan+LSan 与 TSan 构建跑 8 源完整步骤（纯视频、直通、纯视频+FLV、部分 FLV、源停止），没有 sanitizer 报告；服务端正常退出。
- C 的 Release 矩阵子集：`rtmp-h264-aac`、`whip-h264-opus`、`whep-pull`、`whip-chrome-offer`、`whep-pull-chrome-offer` 全部 PASS；`fault-source-stop` 为预期内的失败观测。

## 3. 空闲 Opus→AAC 转码器的内存

### 3.1 现状

- WHIP 与 WHEP Pull 源启动时就创建 `audio_transcoder`，因为固定的 AAC 轨道需要它输出的 `AudioSpecificConfig`（`whip_media_receiver.cpp:83-89`）。
- 主流没有 sink 时只把 `audio_transcoder_active_` 置为 false（`:289-292`），FFmpeg Opus 解码器、libfdk-aac 编码器、重采样器与缓冲都保留。
- 重新有 sink 时调用 `startup()`，它先 `state_.reset()` 再重新初始化（`audio_transcoder.cpp:244`）。也就是说，现有实现在每次重新加入时已经付出了完整的初始化成本，空闲期间保留状态并没有省下什么。

### 3.2 分配量与初始化耗时（微基准）

[`transcoder_mem.cpp`](transcoder_mem.cpp) 与 `build-rel` 的 `libmedia_core.a`、FFmpeg、libfdk-aac、libopus、jemalloc 静态链接。它创建 N 个与服务端配置相同的转码器（48 kHz，64 kbps × 声道），每个转码 100 帧 libopus 编码的正弦波，用 jemalloc `stats.allocated` / `stats.resident` 计量。

| 声道 | N | 初始化后每个 | 转码 100 帧后每个 | `startup()` 首次 | 再次 `startup()` | 每帧转码 |
| --- | --- | --- | --- | --- | --- | --- |
| 2 | 64 / 512 | 585 KiB | 699–700 KiB | 150–160 µs | 97–103 µs | 57–58 µs |
| 1 | 64 / 512 | 534–536 KiB | 635–636 KiB | 139–141 µs | 96–97 µs | 28–30 µs |

释放后 jemalloc 的 `stats.resident` 不会立即下降（512 个立体声释放后仍驻留 329 MiB），所以下面用服务端 PSS 实测，不用分配量代替。

### 3.3 服务端 A/B

B = HEAD 加 [`e-free-idle-transcoder.diff`](e-free-idle-transcoder.diff)：没有 sink 时 `audio_transcoder_.reset()`，有 sink 时重新创建并 `startup()`；删去 `audio_transcoder_active_`。

步骤：只有源 → 64 个 FLV 观众（前 64 个源）→ 观众离开、空闲 → 再加入 64 个 FLV → 空闲。每步先 settle 30 s，再测 30 s。两轮，8 核服务端（`q03`/`q06`；`q03` 中除第一个运行外都因磁盘低于门槛被拦下，在清理后作为 `q06` 全部重跑）。

| 源数 | 阶段 | A PSS MiB | B PSS MiB | A Gcycles/s | B Gcycles/s |
| --- | --- | --- | --- | --- | --- |
| 64 | 只有源 | 82.8–87.1 | 66.4–68.1 | 0.448 | 0.421–0.449 |
| 64 | 64 FLV | 109.4–111.7 | 110.8–114.8 | 2.36–2.42 | 2.34–2.42 |
| 64 | FLV 离开后 | 100.0–102.9 | 64.0–80.5 | 0.446–0.448 | 0.437–0.452 |
| 512 | 只有源 | 537.8–545.5 | 367.1–370.9 | 2.44–2.47 | 2.35–2.46 |
| 512 | 64 FLV | 566.2–574.2 | 415.9–419.6 | 4.11–4.28 | 4.15–4.27 |
| 512 | FLV 离开后 | 554.7–563.5 | 369.0–373.0 | 2.30–2.45 | 2.38–2.44 |
| 1056 | 只有源 | 1164.0–1179.2 | 789.5–791.7 | 4.35–4.36 | 4.43–4.54 |
| 1056 | 64 FLV | 1193.1–1208.8 | 838.5–841.3 | 6.63–6.75 | 6.73–6.76 |
| 1056 | FLV 离开后 | 1181.1–1197.1 | 791.7–794.2 | 4.29–4.42 | 4.46–4.59 |

- 每源净收益：512 源 0.33–0.36 MiB，1056 源 0.36–0.38 MiB；1056 源时 PSS 下降约 33%（约 390 MiB）。
- FLV 离开后，B 在 30 s 内回到只有源时的 PSS，说明释放的内存确实回到了系统，而不只是对象被释放。
- B 的转码器全部被析构：64 源 192 次创建、192 次析构；1056 源 1184 次创建、1184 次析构（`opus_aac.bt` 累计值）。
- 加入 FLV 时 A、B 都启动 64 次 Opus→AAC，有观众时的 CPU 相同，因为 A 本来就会在每次加入时重新 `startup()`。
- 1056 源空闲时，B 的 CPU 两轮都比 A 高 2–4%（4.43–4.59 对 4.29–4.42 Gcycles/s），512 与 64 源没有差异。原因未定位，影响很小。

### 3.4 音频连续性、反复加入与 sanitizer

- **加入测试（`aac_join.py`，Release，`q07`）：** WHIP 与 WHEP Pull 输入各一次，A 与 B 都 PASS。场景是源上已有一个直通 WHEP 观众，FLV、RTSP、RTMP 依次作为唯一的 AAC 消费者加入，中间留出空闲间隔。
  - 每次加入都重启一次转码器；
  - 加入后的输出解码干净；
  - 直通观众的音频没有缺口。
- **源反复启停（`q-lc-asan`、`q-lc-tsan`）：** B 的 ASan+UBSan+LSan 与 TSan 构建都 PASS HEALTHY。测试中 FLV 每 15 s 反复加入、离开，源先后以 SIGTERM 和 SIGKILL 停止，没有观众残留，也没有 sanitizer 报告。
- **加入测试的 sanitizer 对照（`q07`、`q08`）：** B 的 sanitizer 构建跑 WHIP 加入测试时，直通观众回放偶尔出现 1 个 41 ms 的音频时间戳跳变。为此在相同 sanitizer 构建上对 A、B 各重复 3 次：

| 构建 | 出现跳变 | 音频帧数 | UDP RcvbufErrors 增量 | Sanitizer 报告 |
| --- | --- | --- | --- | --- |
| A ASan / TSan | 3 / 6 | 全部 3851 | 0 | 0 |
| B ASan / TSan | 2 / 6 | 全部 3851 | 0 | 0 |

  跳变在 A 中同样出现，帧数没有减少，netns 也没有 UDP 丢包，是 sanitizer 下回放工具的偶发现象，与 B 无关。WHEP Pull 输入的 sanitizer 加入测试 A、B 都没有出现。

### 3.5 判断

- 收益是真实的内存下降：每源约 0.37 MiB，1056 源时 PSS 约 −33%，并且删去了 `audio_transcoder_active_` 这个状态。
- 但它不改变容量：8 核上限由 CPU 决定（上一份报告中 1056 源时最忙核 0.92），1056 源时 PSS 只有约 1.2 GiB。有观众时的 CPU 和加入成本也不变。
- 1056 源空闲时 CPU 有 2–4% 的上升，原因没有定位。
- 结论：本轮不实施。只有在内存成为部署瓶颈时才值得重新考虑，届时直接使用 `e-free-idle-transcoder.diff`，无需再研究。
