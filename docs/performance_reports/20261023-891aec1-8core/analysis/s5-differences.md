## 5 输入、输出与源数量的差异

### 5.1 单路输入开销

`outputs none` 时 runner 仍给每个源保留 1 个 FLV viewer（约 1.8 Minstr/s），下表数值包含它。100 源时服务端只有 1 核左右、频率约 0.8 GHz，空闲进出开销不可忽略。RTMP/RTSP 用 300 源减 100 源得到边际成本：RTMP 5.87 Minstr/源·s，RTSP 6.66。外推截距为空闲基线，约 0.07 Ginstr/s、0.35 Gcycles/s。其余协议用 100 源总量减去该基线再除以源数。

| 输入 | Minstr/源·s（含 1 FLV viewer） | Mcycles/源·s（约 0.8 GHz） | PSS/源 | 结果 | 数据 |
| --- | --- | --- | --- | --- | --- |
| RTMP 发布 | 5.85～5.87 | 4.4～4.6 | 0.7～0.9 MiB | 100/300 PASS | `in-rtmp-100/300` |
| RTSP TCP 发布 | 6.66～6.70 | 4.6～4.7 | 0.8～1.0 MiB | 100/300 PASS | `in-rtsp_tcp-100/300` |
| RTSP UDP 发布 | 6.61 | 4.7 | 1.3 MiB | 100 PASS | `in-rtsp_udp-100` |
| RTMP 拉流 | 5.82 | 6.5 | 0.8 MiB | 100 PASS | `in-rtmp_pull-100` |
| RTSP 拉流 | 7.72 | 7.5 | 1.2 MiB | 100 PASS | `in-rtsp_pull-100` |
| GB UDP | 9.2～10.7 | 5.9～9.0 | 1.0～1.2 MiB | 100 PASS；300 FAIL（UDP 丢包，F6） | `in-gb_udp-100/300` |
| GB TCP active/passive | 9.55 | 8.3 | 1.2 MiB | 100 PASS | `in-gb_tcp_*-100` |
| WHIP（H.264 + Opus） | 134～141 | 62～69（1.2～2.1 GHz） | 2.1～2.2 MiB | 100/150 PASS；200 FAIL（UDP 丢包 98783，F6）；250/300 FAIL（源就绪超时） | `in-whip-100/150/200/250/300` |
| WHEP 拉流（H.264 + Opus） | 221 | 109（1.89 GHz） | 2.4 MiB | 100 FAIL（UDP 丢包 813；F1 使 FLV 半速） | `in-whep_pull-100` |

- 普通输入（RTMP/RTSP/拉流）扣除 FLV viewer 后每源 4～6 Minstr/s，约等于 3～4 个 RTMP viewer。8 核即使接入数百路，输入也只占很小一部分。
- GB 输入扣除 FLV viewer 后每源 7～9 Minstr/s，约为 RTMP 的 2 倍，来自 PS 解复用和 RTP 重组。
- WHIP 和 WHEP 拉流的音频是 Opus，进入内部流前要转成 AAC（`media/codec/audio_transcoder.cpp`，FFmpeg Opus 解码 + 原生 AAC 编码）。每源 140～220 Minstr/s，是 RTMP 输入的 24～38 倍。WHIP 输入稳定容量为 150 路。200 路时服务端只用了满载周期能力的 59%，却因接收丢包失败（F6）；250、300 路在源就绪阶段就超时。限制 WHIP 接入数的是 worker 上同步转码造成的接收延迟，而不是总 CPU。
- RTMP 拉流与 RTMP 发布的指令数相同，周期多 46%（IPC 0.61 对 0.78）。上游实例跑在客户端核上，这部分周期差异来自服务端核上的接收路径，原因未单独分析。

### 5.2 单路输出单 viewer 开销

见第 6.1 节表。按满载周期由低到高：GB TCP（饱和合并写后 1.55）< RTMP 2.52～2.59 ≈ GB UDP 2.53～2.55 ≈ HTTP-FLV 2.57～2.67 < RTSP 2.66～2.80 < WHEP 5.5～5.7 Mcycles/viewer·s。HLS 只有 0.36 Mcycles/viewer·s（0.8 GHz），viewer 按 2 s 分片拉取，服务端共享分片，成本主要是 HTTP 请求处理。

### 5.3 单源与多源

固定 viewer 总数，改变源数（每个源由独立的 RTMP 发布端推流，viewer 均匀分到各源）：

| 负载 | 源数 | Minstr/viewer·s（扣除输入） | PSS | 缺页/s | lag span p50 | 数据 |
| --- | --- | --- | --- | --- | --- | --- |
| RTMP 3000 | 1 | 1.53 | 951 MiB | 84 300 | 131 ms | `ms-rtmp3000-s1` |
| RTMP 3000 | 10 | 1.38 | 659 MiB | 22 100 | 66 ms | `ms-rtmp3000-s10` |
| RTMP 3000 | 100 | 1.31 | 577 MiB | 3 700 | 49 ms | `ms-rtmp3000-s100` |
| RTMP 3000 | 300 | 1.30 | 754 MiB | 940 | 48 ms | `ms-rtmp3000-s300` |
| WHEP 1000 | 1 | 7.45 | 206 MiB | 26 | 204 ms | `ms-whep1000-s1` |
| WHEP 1000 | 10 | 8.15 | 226 MiB | 187 | 97 ms | `ms-whep1000-s10` |
| WHEP 1000 | 100 | 15.0 | 365 MiB | 372 | 61 ms | `ms-whep1000-s100` |

- TCP 输出：单源是内存最差的情况。所有 viewer 在同一时刻为同一个关键帧各分配一份整帧拷贝（3000 × 88 KB ≈ 260 MB 的瞬时分配），分配器只能取新页，稍后再归还，于是每秒 8.4 万次缺页、PSS 偏高。源一多，各源关键帧在时间上错开，释放的内存马上被复用，缺页降到 1/20～1/90，每 viewer 指令少 15%，交付延迟抖动从 131 ms 降到约 50 ms。但容量没有因此提高：`cap3-rtmp-s100` 的稳定容量也是 8500。负载越高，每 viewer 周期越接近单源（8500 时 2.22～2.38 对 2.52～2.59 Mcycles），饱和点附近同样受访存限制（第 4 节）。多源的实际收益是同等负载下更低的 CPU、内存和交付延迟抖动，而不是更高的上限。
- WHEP：源越多越贵。AAC→Opus 按源共享（D03），100 源就有 100 个编码器；(15.0 − 7.45) × 1000 / 100 ≈ 75 Minstr/s 的固定成本落在每个有 WHEP viewer 的源上。单源时它被 1000 个 viewer 摊薄，看不出来。WHEP 多源场景的容量应按“每源 75 Minstr/s + 每 viewer 7.4 Minstr/s”估算。

### 5.4 输出组合

`mix-s1`：单源同时带 RTMP 1000、HTTP-FLV 1000、RTSP 1000、GB UDP 500、GB TCP passive 500、WHEP 300、HLS 300，共 4600 个 viewer。`mix-s50` 把同样的 viewer 分到 50 个源。两轮全部 PASS，所有 viewer 实时比例 ≥0.999。

| 轮次 | 实测 Ginstr/s | 按单协议成本累加的预测 | 差 | Gcycles/s（频率） | PSS | 缺页/s | lag span p50 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `mix-s1` | 9.09 | 9.69 | −6% | 9.23（1.74 GHz） | 990 MiB | 37 400 | 172 ms |
| `mix-s50` | 12.74 | 13.57 | −6% | 11.54（1.93 GHz） | 878 MiB | 1 960 | 55 ms |

预测 = Σ（viewer 数 × 第 6.1 节的每 viewer 指令数）+ Σ（源数 × 输入成本）+ 有 WHEP viewer 的源数 × 75 Minstr/s（AAC→Opus）+ 空闲基线。两轮实测都比预测低 6%，说明各协议输出之间没有额外的相互干扰，同一源上的多种输出只共享输入和派生封装。容量可以按“各协议每 viewer 成本 + 每源固定成本”线性估算，再留出约 6% 的余量。`mix-s50` 比 `mix-s1` 多出的 3.7 Ginstr/s 主要来自 50 个 AAC→Opus 编码器。

容量上，六协议等分的单源混合负载稳定在 7000，7500 为边际（第 4 节）。按单协议周期成本加权平均估算会低估约 9%，因为混合后 IPC 更高。估算混合容量时，用指令数线性相加，再按混合 IPC 约 0.94、满载 21.6 Gcycles/s 换算，比用单协议周期数相加更准确。
