# WHEP source-generation shared RTP 实验报告

## 结论

不保留 candidate。WHEP 的 source-generation shared RTP 在协议语义上可行，真实 Chrome 和双会话 capture 也通过；但 950 路同条件复测中，candidate 的 session queue-full、发送队列尾延迟和 PSS 均明显变差，健康容量没有可信提升。500/900 路只看到轻微 CPU 改善和 900 路内存改善，收益不足以抵消近容量点的排队风险。

实验代码和临时诊断都没有提交，也没有 push；临时 worktree 与实验分支已按约定删除。此文件是保留的结果摘要。

## 初始仓库状态

实验开始时 `main` 工作区干净，当前 Git 实际解析结果为 `HEAD=a64e39cf7257a7622c32c15cb70658de05a296d5`、`origin/main=a64e39cf7257a7622c32c15cb70658de05a296d5`，分支为 `main`。这与任务描述中的 `origin/main=707ca60afc5c38a455272d61de8a54f3775a7394` 不同；全过程以 Git 查询结果为准，未 fetch、改写提交或 push。

## 原有路径与参考实现

control 的 WHEP 路径是：

```text
canonical media_stream
  → 已有 source-generation shared AAC→Opus（需要时）
  → 每个 WHEP session 的 media_reader
  → 每 session webrtc_packetizer（NAL/AU scan、RTP packetization、PT/MID、SSRC/sequence/timestamp、RTCP）
  → 每 session SRTP/SRTCP
  → 每 session UDP queue/socket/endpoint
```

`/home/gyl/data/code/mms-server` 的 WHEP 路径是 `MediaSource → shared MediaBridge → derived MediaSource → RtpMediaSource → N RtpMediaSink`。bridge 对 frame 做一次 RTP packetization，sink 共享完整 `RtpPacket`，再由各 session 分别序列化、SRTP 保护并发送；DTLS、ICE、remote endpoint、RTCP 处理留在各 session。该模型证明 fanout 可以复用 RTP preparation，但它依赖相同 PT/MID/extension layout 和共同 RTP identity。本项目的 WHEP SDP 协商允许这些字段因 viewer 不同而变化，因此没有照搬完整序列化包的共享方式；NACK、FIR、TWCC、REMB、周期 PLI 和大 session buffer 也不在本实验中引入。

## SDP feasibility 与最终边界

H264 在当前 SDP answer 中限定 `packetization-mode=1`，视频时钟为 90 kHz；Opus 时钟为 48 kHz，G711 为 8 kHz。同一 source/profile 的 codec configuration 来自同一轨道 generation，但协商出的 payload type、MID 字符串、MID extension id 可以随 offer 改变；BUNDLE 也要求 MID extension。双 viewer capture 实际使用了不同 PT、MID 和 extension id，因此完整 serialized RTP bytes 不能直接共用。

candidate 采用的最小边界是：

```text
source generation / 已有 shared AAC→Opus
  → shared packetizer 一次：SSRC、sequence、timestamp、marker、payload、fragmentation
  → immutable RTP base packet history
  → 每 session：写入协商的 PT/MID/extmap
  → 每 session：SRTP、SRTCP、RTCP input/PLI、ICE/DTLS、queue/socket/endpoint
```

RTP base 不含协商 extension；session finalizer 写各自 PT/MID。Sender Report 使用共享 source timeline，实际 SRTCP protection 独立；CNAME 在 session 侧恢复为既有 session identity。每个 generation/profile 最多一个 shared producer，复用已有 shared AAC→Opus stream，不创建第二套 converter。producer 由 source worker 持有和执行，没有新增 source-worker → RTP-worker 的 steady-state hop；派生 history 直接向各 session worker 派发。

producer 强持有 source generation，只弱缓存映射；只 fence 实际 packetize 的轨道配置。source replacement 使用新 source 对象和新 RTP state。late viewer 从当前 history 的自然关键帧边界加入，不发 keyframe request。最后一个 viewer 释放 producer，source end 结束所有派生 reader。

## 正确性验证

- Chrome 153.0.8010.52：ICE、DTLS 均 connected；H264 和 Opus 持续到达。首 viewer 收到 943 个 H264 包、解码 60 帧、98 个 Opus 包；晚加入 viewer 使用相同 audio/video SSRC 并解码 32 帧；首 viewer 离开后剩余 viewer 继续解码，累计到 62 帧/102 个 Opus 包。
- source replacement 后新 viewer 获得新 video/audio SSRC，解码 H264 和接收 Opus；旧 viewer teardown clean。Chrome 删除状态为 `[204, 404, 204]`：两个显式存活 session 的 DELETE 为 204；source end 已先移除的 session 后续 DELETE 返回 404。整体 `clean_teardown=true`。
- 双 viewer SRTP 前 capture：重叠视频包 4,799/4,799、音频包 524/524 的 shared fields 完全相同，shared-field mismatch=0、SSRC mismatch=0；两个 viewer 的 PT 和 MID extension id 不同，完整明文 RTP 全部不同，密文相同数为 0。
- 临时 shared packetizer 覆盖通过 H264 FU-A、H265 FU、Opus、G711A/U；验证了 base header 不含 negotiated extension、sequence 连续、AU marker、RTP timestamp step 和音频时钟映射。AAC→Opus 由生产压测 fixture 覆盖，仍使用既有 source-generation shared egress。
- candidate 早期测试发现未协商轨道 config 变化会误结束 producer；已在实验分支修正为仅匹配实际 packetize 的轨道。该修正及 G711 internal PT 映射只用于验证，不进入主线。

## 60 秒 A/B

两侧使用相同 `output_fanout.py`、相同 player/signaling harness、同一 FFmpeg fixture、6 个 worker、100 viewer/s ramp、8 个 client I/O thread、60 秒测量。服务端为 RelWithDebInfo（`-O2 -g -DNDEBUG`、项目 `-Werror`）并链接 jemalloc。FD 为 server/client。500 与 900 的结果来自不含采样诊断的成对轮次；950 表格使用最终候选语义、两侧均启用相同 stage 采样的成对轮次。

| 路数 | 方案 | ready / progressing | CPU cores | PSS KiB | Gbit/s | pps | queue-full | FD server/client | viewer B/s min / p10 / p50 / p90 / max | min video/audio packets |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---|---:|
| 500 | control | 500/500 | 2.228 | 200,404 | 1.1134 | 251,982 | 0 | 529/532 | 278,265 / 278,348 / 278,350 / 278,370 / 278,432 | 27,181 / 3,000 |
| 500 | candidate | 500/500 | 2.151 | 200,127 | 1.1133 | 251,891 | 0 | 529/532 | 278,331 / 278,331 / 278,331 / 278,331 / 278,331 | 27,178 / 3,000 |
| 900 | control | 900/900 | 4.945 | 509,890 | 1.4034 | 319,241 | 0 | 929/932 | 186,033 / 188,676 / 194,454 / 199,203 / 201,253 | 18,349 / 1,953 |
| 900 | candidate | 900/900 | 4.823 | 452,270 | 1.4121 | 321,738 | 0 | 929/932 | 191,951 / 192,959 / 196,438 / 198,894 / 200,153 | 18,997 / 2,023 |
| 950 | control | 950/950 | 5.120 | 877,846 | 1.2952 | 294,463 | 0 | 979/982 | 165,771 / 166,988 / 170,581 / 173,755 / 178,551 | 16,331 / 1,773 |
| 950 | candidate | 950/950 | 5.097 | 990,873 | 1.3142 | 298,881 | 332,456 | 979/982 | 164,474 / 166,971 / 173,353 / 177,278 / 178,980 | 16,355 / 1,791 |

表内 CPU/PSS 是服务端指标。harness 的结果文件也记录 client/player CPU、PSS、FD，但当前保留的本轮专用原始结果只有 500 路 control：2.382 client cores、53,377 KiB client PSS、532 client FD；candidate 与 900/950 路对应结果随临时 worktree 清理，现无法恢复。因此这些轮次没有可核对的 client CPU/PSS 成对数据，不能据此声称客户端资源持平。

500 路 CPU 约降 3.4%、PSS 基本不变、吞吐不变；900 路 CPU 约降 2.5%、PSS 降约 11.3%、吞吐增约 0.6%。950 路 candidate 的 aggregate throughput 高约 1.5%，但 CPU 仅低约 0.5%，PSS 高约 12.9%，queue-full 从 0 升至 332,456；最低 viewer B/s 低约 0.8%。950 路重复轮次也有机器/拥塞波动：control queue-full 为 0 到 45,554，candidate 为 332,456 到 419,093。candidate 没有显示出可信的健康 viewer ceiling 提升，因此没有继续测 1000/1050。

## 调度与发送延迟

stage histogram 每 32 个 session frame 采样，百分位是指数桶上界，max 是实测最大值。900 路 30 秒同构采样中，source-ready→viewer callback control p50/p90/p99/max 为 8.4/134.2/268.4/629.8 ms；candidate input-ready→viewer callback 为 8.4/67.1/268.4/653.5 ms，shared-ready→viewer callback 为 4.2/67.1/134.2/651.0 ms，未见 callback scheduling tail 明显恶化。

950 路最终成对采样：

| stage | control p50 / p90 / p99 / max | candidate p50 / p90 / p99 / max |
|---|---|---|
| input-ready→shared processor start | — | 4.2 / 33.6 / 67.1 / 192.4 ms |
| shared processor duration | — | 4.1 / 16.4 / 32.8 / 0.069 ms |
| input/source-ready→viewer callback | 33.6 / 134.2 / 536.9 / 797.4 ms | 33.6 / 134.2 / 268.4 / 798.3 ms |
| shared-ready→viewer callback | — | 16.8 / 134.2 / 268.4 / 771.8 ms |
| viewer callback→RTP header finalized | 1.0 / 2.0 / 2.0 / 0.040 ms | 0.0005 / 0.524 / 1.049 / 7.206 ms |
| packet/header ready→SRTP complete | 8.2 / 32.8 / 65.5 / 1.947 ms | 8.2 / 32.8 / 65.5 / 2.856 ms |
| SRTP complete→async send submit / queue wait | 536.9 / 2,147.5 / 2,147.5 / 2,480.3 ms | 536.9 / 4,295.0 / 8,589.9 / 5,138.0 ms |
| async send submit→completion | 0.524 / 4.194 / 67.109 / 198.544 ms | 0.524 / 4.194 / 67.109 / 158.617 ms |

共享 packetization 本身耗时很短，但近 knee 时 candidate 在 per-session send queue 等待的 p90/p99 约为 control 的 2/4 倍，最大等待也约翻倍。这与 queue-full 上升和 PSS 增长一致，是否决的主要证据。perf 的 CPU 采样受 `perf_event_paranoid=4` 限制，bpftrace 也不可用；没有伪称获得函数级 CPU 分解。代码路径确实把 N 次 NAL/RTP packet construction 改为一次，但总 CPU 只小幅下降。

## 回归与 sanitizer

- fresh RelWithDebInfo build 完成；确认 `-O2 -g -DNDEBUG`、`-Werror`，`ldd` 显示 `libjemalloc.so.2`。
- 全量 `ctest --test-dir build --output-on-failure`：222/222 通过。最终 G711 shared payload 映射及 packetizer 覆盖加入后，又以标准和 UBSan build 各重跑 `media_pipeline_tests`、`webrtc_signaling_tests` 两项，均通过。
- `go test ./...` 与 `go vet ./...` 均通过。
- UBSan：完整 `webrtc_signaling_tests`、最终 shared RTP identity 两 viewer harness，以及最终 `media_pipeline_tests`/`webrtc_signaling_tests` focused CTest 通过。
- ASan 未运行：仓库 CMake guard 因当前 Boost.Context library 不具备 sanitizer-enabled `context-impl=ucontext` 而拒绝配置；遵守该 guard，没有绕过。
- multi-protocol regression 由全量 222 项 CTest 覆盖；candidate 没改动其他输出协议。

## 最终处置

该实验验证了共享 RTP base 的协议可行性、Chrome 互通和 generation 生命周期，但性能 A/B 不满足保留条件。实验分支/worktree 清理后，`main` 应保持初始 HEAD，`origin/main` 按实际 Git 查询保持原值；无实验提交，push = none。
