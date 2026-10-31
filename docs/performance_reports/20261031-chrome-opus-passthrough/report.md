# GB PS 尾部完整性、RTSP AAC XFAIL 与 Chrome Opus 直通收尾（2026-10-31）

## 结论

本轮完成上一份报告（[`20261031-gb-ps-rtsp-aac-chrome-opus`](../20261031-gb-ps-rtsp-aac-chrome-opus/report.md)）批准的三项收尾，没有混入其他生产改动。

| 项 | 结果 |
| --- | --- |
| GB PS 尾部完整性 | `gb_ps_demux` 改为按完整 PS pack 送入 libmpeg，并核对“完整 pack 数 = 帧数 + 1”。新增 3 个截断变异。旧工具在 PCMA/PCMU 上把“视频 pack 缺尾、RTP 序号连续”误判为 PASS；新工具在 4 份 dump 上全部正确判定，正常截在最后一个 pack 中间的捕获仍然接受 |
| RTSP AAC 已知问题 | 矩阵对 3 个 RTSP AAC 场景做严格 XFAIL：锯齿签名必须是唯一失败；锯齿消失报 XPASS 并计为失败。13 种人工变异全部按预期分类；Release 全矩阵 30 PASS + 3 XFAIL |
| Chrome 默认 WHEP Opus 直通 | 按实验补丁实施为生产改动，并更新 `docs/protocols.md`。Release 矩阵、真实 Chrome、ASan+LSan、TSan 回归和 8 核容量回归见下文 |

## 1. 版本与环境

| 项 | 值 |
| --- | --- |
| 源码 | `aa083a5` 加工作区改动，`media_server --version` 为 `aa083a56479c-dirty` |
| Release | RelWithDebInfo，GCC 16，hardening ON，benchmarks ON；`media_server` sha256 前缀 `0e771e53d4d9a436` |
| Sanitizer | Debug 的 address、thread 构建（`build-san-address`、`build-san-thread`）；TSan 对照另编一份只回退 `webrtc_sdp.cpp` 的 HEAD 等价版本 |
| 测试机 | 与上一份报告相同：8 核服务端（CPU 0–7），客户端在 8–15、24–31，独立网络命名空间 198.19.0.10/11 |
| 作业 | [`jobs/`](jobs/) 下 `p01`–`p05`；原始结果在测试机 `runs4/p-*`，媒体 dump 跑完即删除，只保留 JSON 和日志 |

## 2. GB PS 尾部完整性

### 2.1 问题

- libmpeg 的 `ps_demuxer_input()` 总是返回输入长度：不完整的头会暂存在内部 buffer，不完整的 PES 会留在内部 packet 里。因此原工具的 `pending` 每次都被清空，输出里的 `trailing_bytes` 恒为 0，没有意义。
- libmpeg 会一直留住最后一个 H.264/H.265 访问单元，等下一个访问单元开始才输出。
- 服务端每个媒体帧封成一个 PS pack，一次写出（`mpeg_ps_output::write_packet` 只写一次），再由 librtp 切成同一 RTP 时间戳的多个包；首包以 `00 00 01 BA` 开头。librtp 的 PS 打包把 marker 恒置 0，不能用来分帧。
- 中途某个视频 pack 丢了尾包、而 RTP 序号连续时（例如发送端截断），旧工具没有任何检查能发现：视频解码检查只比对已输出的帧，G.711 素材比对不涉及视频。

### 2.2 修改

[`bench/gb_ps_demux.cpp`](../../../bench/gb_ps_demux.cpp)：

- 以 RTP payload 开头的 pack header 为界，累积一个完整 pack 后才送入 libmpeg；
- 最后一个 pack 可能被捕获窗口截断，只计入新增的 `trailing_rtp_packets` 和 `trailing_bytes`，不作为错误；
- 第一个 pack header 之前出现 RTP 包时报错；
- 要求 `ps_packs == video_frames + audio_frames + 1`，其中 1 是 libmpeg 留住的最后一个视频访问单元。

没有重新实现 PS 解复用，也没有改 libmpeg。

5 份真实 dump（4 种编码 × 3 种传输中的 4 份，以及 RTSP AAC 分层调查的 GB dump）都满足这个等式。

| dump | RTP 包 | 完整 pack | 视频帧 | 音频帧 | 尾部包/字节 |
| --- | --- | --- | --- | --- | --- |
| H.264+AAC，TCP passive | 10430 | 2931 | 1143 | 1787 | 6 / 7120 |
| H.264+PCMA，TCP active | 10400 | 2923 | 1140 | 1782 | 1 / 199 |
| H.264+PCMU，UDP | 10407 | 2925 | 1141 | 1783 | 1 / 198 |
| H.265+AAC，TCP active | 10604 | 2924 | 1140 | 1783 | 17 / 19931 |
| RTSP AAC 分层 | 7172 | 2002 | 780 | 1221 | 6 / 6497 |

### 2.3 负向测试

[`regression/gb_ps_negative.py`](../../../regression/gb_ps_negative.py) 新增三个变异：

- `video_pack_tail_removed`：去掉中段一个视频 pack 的最后一个 RTP 包，并重排序号，使 RTP 连续；
- `capture_ends_mid_pack`：在最后一个多包 pack 的首包后截断，属于正常捕获结束，**必须接受**；
- `capture_ends_mid_record`：截断最后一条 RFC 4571 记录。

先用旧 demux、后用新 demux 各跑一遍：

| dump | 旧 demux | 新 demux |
| --- | --- | --- |
| H.264+AAC | PASS（缺尾被视频解码检查发现） | PASS |
| H.264+PCMA | **FAIL：`video_pack_tail_removed` 被接受** | PASS（`2923 whole PS packs produced 2921 frames`） |
| H.264+PCMU | **FAIL：`video_pack_tail_removed` 被接受** | PASS |
| H.265+AAC | PASS | PASS |

原有 7 个变异的结果不变，`capture_ends_mid_pack` 在新旧工具下都被接受。

## 3. RTSP AAC 严格 XFAIL

[`regression/protocol_matrix.py`](../../../regression/protocol_matrix.py)：

- 只对 `rtsp-tcp-h264-aac`、`rtsp-udp-h264-aac`、`rtsp-tcp-h265-aac` 生效。
- 判为 XFAIL 必须同时满足：
  - perf_run 为 FAIL/UNCONFIRMED，没有 errors，所有组都 ready、progressing 且覆盖率 ≥0.99；
  - 没有实时落后组；decode_probe 全部通过；没有 churn 失败；
  - 每个失败的 av_probe 都是 GB 输出，且只有 `audio_timeline` 失败、编码为 AAC、没有重叠；
  - 缺口数 ≈ 帧数/33（±1.5）；
  - 最小步长误差 ≥ −1/3 ms，最大 ≤ 32/3 ms（各含 0.01 ms 容差）；
  - 记录的每个缺口都是 32/3 ms，间隔恰为 33 帧。
- 场景 PASS 时报 XPASS，并与 FAIL 一样使矩阵失败。
- 锯齿常数由机制推出：33 帧/包 × (1024/48 − 21) ms。没有放宽 `gb_av_probe.py` 的全局 5 ms 阈值。

用上一轮 `mx-full` 的真实结果和人为变异核对分类器（`xfail_check.py`，未入库）：

| 输入 | 分类 |
| --- | --- |
| 3 个 RTSP AAC 真实结果 | XFAIL |
| 其余 28 个真实结果 | 与原状态相同 |
| 场景名不在名单内 | FAIL |
| 缺口改为 12 ms | FAIL |
| 一个缺口偏离 33 帧周期 | FAIL |
| 出现重叠 | FAIL |
| 缺口数翻倍 | FAIL |
| 音频解码失败 / 素材比对失败 / 音频缺失 | FAIL |
| RTMP 输出 av_probe 失败 | FAIL |
| decode_probe 失败 | FAIL |
| 观众未 progressing | FAIL |
| 服务端不稳定 | FAIL |
| 所有探测都通过 | XPASS |

## 4. Chrome 默认 WHEP Opus 直通

### 4.1 改动

[`media/webrtc/webrtc_sdp.cpp`](../../../media/webrtc/webrtc_sdp.cpp)，与实验补丁 [`d-passthrough-experiment.diff`](../20261031-gb-ps-rtsp-aac-chrome-opus/d-passthrough-experiment.diff) 相同：

- `opus_passthrough_compatible()` 不再要求 2 声道源的 offer 带 `stereo=1`；
- `maxplaybackrate`、`maxaveragebitrate`、`a=maxptime` 的限制原样保留；
- 直通时 `sprop-stereo` 按源轨道声道填写，egress 时按实际编码声道填写。所有入口的 G.711 轨道都固定为 1 声道，因此对 G.711 没有行为变化。

`docs/protocols.md` 已写明：
- RFC 7587 §7.1 多目的地情形下 SHOULD NOT 的偏离范围；
- 每观众约 38 kbps 的带宽代价、Chrome 下混播放；
- 切换点的隐藏。

`webrtc_sdp.cpp` 在第 1284 行附近另有 7 处 clang-format 违规，HEAD 上就已存在，与本次改动无关，未顺手修改。

### 4.2 Release 协议矩阵（p01）

33 个场景（原 31 个，加上 `whip-chrome-offer`、`whep-pull-chrome-offer`）：30 PASS + 3 XFAIL，矩阵退出码 0。

- 新增两个场景让 WHIP 和 WHEP 拉流源的 WHEP 观众使用 Chrome 默认 fmtp，同时挂一个 HTTP-FLV 观众（AAC 订阅者）：WHEP 解码与音画同步通过（A/V −4.3 ms），FLV 解码通过。
- 矩阵中的服务端日志是 info 级别，转码器启动属于 debug 日志，所以矩阵不能用来判断走的是直通还是 egress；这一点由第 4.3 节的 TOC 和第 4.5 节的 bpftrace 证明。

### 4.3 真实 Chrome（p02，`p-browser-p`）

脚本、源与观众设置与上一份报告第 D 节相同：
- 源：单声道 `mono`、立体声 `lr`（右声道 −12 dB）、真实 Chrome 推流 `chrome_pub`（运行中切换 `stereo=1`/`stereo=0`）；
- 观众：`whep_fanout` 的 5 种 offer、真实 Chrome 观众（WebAudio 测左右 RMS）、第 45 s 加入的 HTTP-FLV 观众；
- 结束方式：SIGKILL、SIGTERM、WHIP DELETE。

| 检查 | 结果 |
| --- | --- |
| Chrome 默认 offer，`mono` | 直通，`sprop-stereo=1`，TOC 4551 帧全单声道；Chrome 实测播放右/左 = 1.0；无隐藏、无丢包 |
| Chrome 默认 offer，`lr` | 直通，TOC 全立体声；Chrome 播放右/左 = 1.0（下混） |
| `stereo=0` / Firefox 类 offer，`lr` | 直通立体声，与实验构建一致 |
| `maxaveragebitrate=32000`、`maxplaybackrate=16000`、`a=maxptime:60` | egress 单声道，`sprop-stereo=0`；服务端启动 7 个 AAC→Opus 1ch，正好是受限 offer 的数量 |
| `chrome_pub` 声道运行中变化 | 直通观众 TOC：0–30 s 单声道，切到 `stereo=1` 后立体声，切回 `stereo=0` 后单声道；受限 offer 观众始终是 egress 单声道 |
| 切换点 | Chrome 观众在 30.6 s 和 61.2 s 各出现隐藏，共 3 次、450 样本（9.4 ms），丢包 0 |
| 回放解码（whep_fanout dump） | `mono`、`lr` 无警告，A/V −3.3 ms；`lr firefox` 有 1 个音频缺口，但该观众与同源 `stereo0` 观众收到的包数相同（视频 42437，音频 4500），属于回放工具的偶发误差，不是服务端问题 |
| AAC 观众动态加入 | `lr`、`chrome_pub` 第 45 s 加入 FLV，按需启动 Opus→AAC 共 6 次，FLV 解码无缺口，A/V −2.8 ms |
| 源停止与清理 | DELETE 返回 204；WHEP 会话 21 个全部关闭，6 个随源结束；receivers 22.1 s 内清空（SIGKILL 的源靠 ICE 超时）；服务端存活 |

### 4.4 Sanitizer

| 运行 | 结果 |
| --- | --- |
| ASan+LSan 矩阵子集（p03）：`whip-h264-opus`、`whep-pull`、`whip-chrome-offer`、`whep-pull-chrome-offer`、`rtsp-tcp-h264-opus`、`fault-source-stop` | 全部 PASS（`fault-source-stop` 为预期失败观察到）；8 个 sanitizer 日志都只有 Boost.Context makecontext 提示，同时证明日志通道有效 |
| TSan 矩阵子集（p03） | 同上 6 个场景全部 PASS；没有 TSan 报告文件 |
| ASan+LSan 真实浏览器（p02，`p-browser-asan`，60 s） | 退出码 0；服务端收到 SIGTERM 后 10 s 内正常退出，`server.log` 中只有 makecontext 提示，没有 ASan 或 LSan 报告 |
| TSan 真实浏览器（p02，`p-browser-tsan`） | 失败：3 个 WHIP 源并发创建时，`whip::create()` 生成 DTLS 证书，在 OpenSSL `ossl_ht_get`/`ossl_ht_insert` 上报告数据竞争；随后主线程恢复 accept 协程时在固定地址 0x6f8d 发生 SEGV，Chrome 推流收到 connection reset |
| TSan 对照（p05）：HEAD 等价构建与补丁构建各跑 2 次 | 两边各有 1 次出现相同的竞争和 SEGV，签名一致（同一 `whip.cpp:135` 调用点、同一地址）；另外 1 次都干净。干净的补丁运行与 Release 结论一致：Chrome 默认 offer 直通、受限 offer egress 7 个、动态 AAC 6 个、21 个 WHEP 会话全部关闭、receivers 27.3 s 内清空 |

TSan 失败在 HEAD 上同样能复现，与本补丁无关。它出现在 TSan 下的 OpenSSL 内部哈希表和 Boost.Context 协程切换处，Release 与 ASan 运行都未出现（包括 1056 源并发建 WHIP），本轮不扩大处理；如需跟进，单独排查 OpenSSL 3.5 证书生成在 TSan 下的锁检测，以及 TSan 对 ucontext 协程的支持。

### 4.5 8 核容量回归（p04）

测量方法与上一份报告相同：WHIP 输入，每源 2 个 Chrome 默认 offer 的 WHEP 观众，60 s 窗口，`perf stat` 加 bpftrace 转码器计数。

| 运行 | 结果 | Gcycles/s | Ginstr/s | 服务端核 | 最忙核 | 控制 p99 | PSS | 窗口内 egress / Opus→AAC |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 64 源 ×2，第 1 轮 | PASS | 1.44 | 1.49 | 1.53 | 0.223 | 3.4 ms | 142 MiB | 0 / 0 |
| 64 源 ×2，第 2 轮 | PASS | 1.41 | 1.49 | 1.49 | 0.191 | 3.0 ms | 140 MiB | 0 / 0 |
| 1056 源 ×2，第 1 轮 | PASS HEALTHY | — | — | 7.25 | 0.917 | — | 2022 MiB | — |
| 1056 源 ×2，第 2 轮 | PASS HEALTHY | — | — | 7.38 | 0.934 | — | 2010 MiB | — |

- 64 源的结果与实验构建一致（实验 1.44 Gcycles/s、最忙核约 0.21），基线为 4.32 Gcycles/s、0.667；全程没有创建 egress，转码器计数为 0。
- 1056 源两轮 HEALTHY，2112 个观众全部 ready 并持续推进，UDP 丢包 0；最忙核 0.917–0.934，与实验构建的 0.916–0.920 相当，PSS 相同（约 2.0 GiB）。
- 本轮只复核实验构建已确定的边界，没有重新二分搜索；1088 源在实验构建上失败。

## 5. 文件

| 文件 | 说明 |
| --- | --- |
| `bench/gb_ps_demux.cpp` | 按完整 pack 解复用、帧数核对与尾部计数 |
| `regression/gb_ps_negative.py` | 新增 3 个截断变异 |
| `regression/protocol_matrix.py` | RTSP AAC 严格 XFAIL/XPASS，新增 2 个 Chrome offer 场景 |
| `media/webrtc/webrtc_sdp.cpp` | Chrome 默认 offer 直通 |
| `docs/protocols.md`、`docs/performance.md` | 直通条件与 RFC 7587 取舍；demux 尾部规则、负向变异、XFAIL 规则 |
| [`jobs/`](jobs/) | 测试机作业 p01–p05 |
