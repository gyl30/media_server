# AAC 编码性能与 WHEP 高容量优化（2026-10-29）

- BASE_HEAD：`415e8aa`（任务描述称 worker 均衡“已推送”，实际 `415e8aa` 及之前的本地提交均未推送）。
- FINAL_HEAD：`24c80ec`。全部提交仅在本地 main，未推送、未部署。
- 测试机：root@<TEST_HOST>（Xeon Silver 4216），netns `msperf-20261013T011210Z-7788320`。服务端固定 cpu 0-7（8 worker），客户端和上游使用 8-15、24-31，HT 兄弟 16-23 空闲。
- 负载口径沿用 [20261029-whep-pull-optimize](../20261029-whep-pull-optimize/report.md)：WHEP Pull 每路 1 个 FLV 观看者，夹具为 720p30 H.264 + AAC，上游为另一个 media_server（16 worker，`--upstream-sources 64` 时多路拉流共享 64 个上游源）。
- 每次 A/B 交替运行，上游二进制固定为 rt-e1（`--upstream-bin`），只替换被测服务端。

## 结论

**在固定 8 核下，WHEP Pull 的稳定容量（最忙 CPU ≤0.93）：**

| 版本 | 单路成本（160 路） | 稳定容量 | 最大持续容量（≤0.98，60 s 实时、无丢包） | 可否进入正式代码 |
| --- | --- | --- | --- | --- |
| 正式版（ffmpeg901 GPLv3 SDK，HEAD 代码） | 104.5 Mcyc/s | 160 | 168（上一轮，本轮 160 路时最忙 0.91） | 现状 |
| 最小 LGPL SDK + pow43 补丁（原生 AAC） | 97.7（−6.5%） | **176** | **184**（192 失败） | **可以**：输出逐字节相同，构建可复现，无新许可风险 |
| 最小 LGPL SDK + libfdk-aac | 59.0（−43.5%） | **300** | **320**（336 失败） | 代码已就绪（编译期开关，默认关闭）；**许可和专利需外部确认** |
| libfdk-aac + afterburner=0（fdk 构建的固定行为） | 34.6（−67%） | **496** | **520**（536 失败） | 已提交（`MEDIA_SERVER_AAC_ENCODER=libfdk_aac` 时生效）；初步试听无差异，仍需许可确认 |

- **可以直接进入正式代码的**（已提交）：
  - idle timer 归属修复；
  - WHEP 播放会话和共享转码器的最少负载放置（交错负载下 s400w400 从 FAIL 变为 PASS）；
  - 转码时间线扣除编码器延迟（WHIP 音画偏差 −25.8 → −4.7 ms）；
  - 最小 SDK 的可复现构建与 pow43 补丁；
  - AAC 编码器的编译期选择，以及 fdk 需要的全局头设置。
  - 采用 pow43 需要部署方改用 `deps/ffmpeg/build.sh --patched` 构建的 SDK。这同时会让服务端摆脱现有 GPLv3 SDK（x264/x265 等），但仍要处理静态链接的 LGPL 合规问题（3.4）。
- **性能优秀但有风险的**：
  - libfdk-aac：技术上已验证，CPU −43.5%、容量 +88%，生产档位的客观质量更好。风险在于 FDK 许可要求随附源码，且不授予专利许可，FSF 也有专门警示；AAC 专利是否覆盖服务端转码，需要外部确认。不能默认放进公开发行包。
  - afterburner=0：在 fdk 的基础上再省 41%，容量达到 520。生产档位的客观指标仍好于原生，但低码率下的 SNR 与原生持平。用户初步试听时，三种编码器在 128k 和 64k 下都听不出区别，因此音质不构成采用障碍，决定取决于许可。
- 播放侧（与 Pull 输入分开报告）：约 400 个单观看者 AAC 源（每源 44.5 Mcyc/s，均衡后各线程 0.97）。

## 1. WHEP Pull worker 放置的正确性审查

审查 `a636699`/`415e8aa` 把 WHEP Pull 会话放到最少会话 worker 后的所有权：

| 对象 | 归属与时序 | 结论 |
| --- | --- | --- |
| resolver、UDP socket、DTLS、SRTP、DTLS/keepalive timer | 构造时绑定所选 worker 的 `io_context`，之后只在 `run()` 协程内使用 | 正确 |
| idle timer | `startup()` 在 HTTP 线程上 `idle_timer_.start()`，同时 `run()` 已在所选 worker 上 spawn，二者可能并发访问同一 timer | **缺陷，已修复**（`69e8445`，移到 `run()` 开头） |
| worker 计数 | 选 worker 与登记在同一 mutex 内完成；析构时在 mutex 内递减 | 原子、失败路径归还 |
| 析构与 worker 生命周期 | 会话由协程持有，worker 关闭先停 `io_context` 再析构 | 无悬挂 |
| WHIP 同类路径 | timer 在 spawn 前布置，登记在 startup 之后 | 无同类问题，未改动 |

- 回归验证：idle timer 竞争窗口是微秒级，两次 TSan 压测（4 对 20 s、16 对 60 s 的创建/删除竞争）都没有复现，因此 RED 只能依据代码审查确认。修复后 `pull_lifecycle` 在 Release 和 TSan 下通过，上游被杀后 20 s 由 idle timer 清理；TSan 60 s / 16 对压测无报告（`san/runs/pull-*`、`race-*`）。
- 选择 worker 与登记之间不是原子的（whip/whep 都是先选再登记）。这只影响突发时的均衡，测量中看不到影响，未改动。

## 2. FFmpeg 原生 AAC 编码器：ESC 码本 x^(4/3) 查表

- 补丁：[`deps/ffmpeg/patches/0001-aacenc-escape-pow43-table.patch`](../../../deps/ffmpeg/patches/0001-aacenc-escape-pow43-table.patch)。ESC 码本量化误差估计中的 `c*cbrtf(c)` 改为 8192 项表，表用 `AVOnce` 只初始化一次。索引来自 `av_clip_uintp2(..., 13)`，范围为 [0, 8191]。
- 可复现构建：[`deps/ffmpeg/build.sh`](../../../deps/ffmpeg/build.sh) 使用固定的上游 tarball 并校验 sha256（FFmpeg 9.0.1 另经 PGP 签名验证），生成最小的 LGPL 2.1+ 静态 SDK，不修改 `~/ffmpeg901` 和 `third/`。同一脚本重建两次后，去掉调试信息比较：各库中只有含 `--prefix` 字符串的 `version.o` 不同，其他目标文件逐字节一致，libopus 整库一致（`raw/repro/`）。
- 逐字节等价：在 1867 个用例上，补丁版与未补丁版输出完全一致（`raw/aacenc/equiv-pow43*.json`，非 PIC 与 PIC 两套 SDK 各一次）。用例覆盖 silence/sine/sweep/noise/impulse/square/fullscale/overrange/denormal/tiny/NaN/Inf/mixed 13 种信号 × 10 个采样率 × 单/双声道 × 4 档码率、19 段真实音乐 × 8 种采样率/声道 × 多档码率，以及 cutoff/aac_coder/pns/is/tns/ms/profile/pce 选项。状态分布为 1595 ok、176 encode_error、96 open_error，两侧完全相同，解码错误 0。
- 单编码器周期数（测试机，cpu 3，fx01 48k/2ch/128k，cutoff 20000）：2.744 G → 2.380 G cycles（−13.3%），md5 相同。
- 打补丁后的热点：twoloop 占 35%，分布平坦，最热的一条指令 2.66%；ESC 占 12.7%，UQUAD/SQUAD/UPAIR 各约 6–7%。在保持逐字节一致的前提下，没有再找到低成本的收益，因此停止继续优化原生编码器。

### 2.1 服务端 A/B

同时段交替测量，WHEP Pull 160 路 FLV，`--upstream-sources 64`，perf 周期数取 2–3 轮（`raw/runs/{k,m,h}-*`）：

| 变体 | SDK | 每路 Mcyc/s | 最忙 CPU | PSS MiB | 控制 p99 ms |
| --- | --- | --- | --- | --- | --- |
| e1 | ffmpeg901（GPLv3，gcc 16.0.1） | 104.5 | 0.91 | 339 | 20–22 |
| fu | 最小 SDK，未打补丁（gcc 16.2） | 108.9–109.3 | 0.95 | 336–339 | 20–25 |
| fp | 最小 SDK + 补丁（gcc 16.2） | 97.3–97.5 | 0.84 | 336–338 | 15–20 |
| gp | 最小 SDK + 补丁（gcc 16.0.1） | 97.6–97.7 | 0.85 | 336 | 15–21 |
| fp2 | fp + 时间线修正 + 全局头 | 97.3 | 0.84 | 337 | 15 |

- 相对生产 SDK，补丁带来 −6.5%；相对同配置的未打补丁最小 SDK，是 −10.5%。fu 比 e1 慢约 4%，但打补丁后两种编译器的结果一样（fp 97.4 对 gp 97.7）。这说明差异只出现在未打补丁的 cbrtf 路径上，具体原因没有隔离。
- fp2 等于 fp，说明时间线修正和全局头没有带来可测量的开销。
- 容量：gp mix176 最忙 0.92（fp），mix184 0.97 PASS（fp、gp 各 1 轮），mix192 FAIL（最忙 1.00，丢包 85 万）。

## 3. libfdk-aac 独立接入

### 3.1 接入方式

- `deps/ffmpeg/build.sh --fdk-aac` 构建 fdk-aac 2.0.3（sourceforge 官方 tarball，sha256 829b6b89…，md5 与 SourceForge RSS 公布值一致），然后以 LGPL FFmpeg + `--enable-libfdk-aac` 方式接入。配置结果为 `License: LGPL version 2.1 or later`，不需要 `--enable-nonfree`；但这只说明 configure 允许这样组合，不能作为许可证兼容的证明，见 3.4。
- `MEDIA_SERVER_AAC_ENCODER`（CMake 缓存变量）在编译期选择 `aac`（默认）或 `libfdk_aac`。原生编码器仍是默认，运行时不做回退。
- 接入时发现，fdk 只有在设置 `AV_CODEC_FLAG_GLOBAL_HEADER` 时才输出 AudioSpecificConfig，否则所有 WHEP Pull 都会因 `aac encoder missing codec config` 启动失败（`san/runs/pull-fdk-rel-r1`）。现在 AAC 编码统一设置该标志，原生编码器输出的 md5 不变。
- 两种编码器的编码器延迟（用互相关测量，与 `initial_padding` 一致，5 种采样率/声道组合）：原生 1024 样本，fdk 2048 样本。

### 3.2 质量

工具：`tools/aacenc/quality.py`。19 段素材为 18 段 `~/Music` 片段加 1 段性能夹具音频，覆盖 HiFi 人声、轻音乐器乐、DJ、粤语、红歌、草原、古风、邓丽君、民谣 HiFi，每段 30 s。编码后用 FFmpeg 解码，按测得的延迟对齐后计算以下指标（均为 19 段的平均值）：

| 采样率-声道-码率 | 编码器 | SNR dB | 分段 SNR dB | LSD(50 Hz–8 kHz) dB | 带宽 Hz（源） | 实际 kbps | 编码 ms/秒音频 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 48k-2-128k（生产） | aac | 20.64 | 22.03 | 5.61 | 12834（12929） | 130.0 | 19.0 |
| | fdk | **26.71** | **27.67** | **4.71** | 12823 | 128.4 | **9.8** |
| 48k-1-64k（生产单声道） | aac | 14.85 | 17.44 | 7.58 | 13183 | 64.5 | 8.6 |
| | fdk | **22.51** | **24.66** | **6.61** | 12889 | 64.2 | **4.9** |
| 44.1k-2-128k | aac | 20.93 | 22.27 | 5.59 | 12701 | 130.1 | 18.4 |
| | fdk | **26.75** | **27.70** | **4.62** | 12749 | 128.3 | **9.4** |
| 48k-2-96k | aac | 16.60 | 17.95 | 7.73 | 12791 | 96.9 | 21.9 |
| | fdk | **21.31** | **23.43** | 7.77 | 12810 | 96.4 | **10.2** |
| 48k-2-64k（低码率） | aac | 10.77 | 11.48 | **8.82** | 11931 | 63.6 | 33.2 |
| | fdk | **16.39** | **17.64** | 11.96 | 12655 | 64.4 | **10.3** |
| 48k-1-32k | aac | 8.70 | 9.22 | **9.21** | 12375 | 31.1 | 16.4 |
| | fdk | **13.88** | **15.70** | 12.62 | 12372 | 32.2 | **4.7** |
| 32k-2-64k | aac | 15.46 | 16.67 | **8.75** | 10798 | 64.5 | 14.2 |
| | fdk | **18.58** | **20.66** | 9.56 | 11406 | 64.4 | **6.9** |
| 16k-1-32k | aac | 15.03 | 17.51 | **7.93** | 7118 | 32.2 | 4.1 |
| | fdk | **21.50** | **22.52** | 11.26 | 7176 | 32.2 | **2.1** |

- 生产档位（48k 下每声道 64 kbps）和 44.1k/128k 档位：fdk 在 SNR、分段 SNR、带内 LSD 三项上都更好，编码速度约为原生的 2 倍，码率更贴近目标。
- 低码率档位：fdk 的 SNR 高 5–6 dB，但 LSD 更差。fdk 在低码率下更积极地使用噪声替代等参数化工具，波形误差小，而逐帧频谱差异大。这两项客观指标给出的结论相反；初步试听没有听出区别（见下条）。
- 边界信号（`raw/aacenc/edge-fdk.json`）：13 种信号 × 7 个采样率 × 单/双声道 × 4 档码率，fdk 728/728 正常，原生 616 正常；原生的 112 个 encode_error 全部来自 NaN/Inf 输入，两者都没有解码错误。media_server 的 AAC 编码输入来自 Opus 解码，不会出现 NaN/Inf。
- 主观听感（用户初步试听，2026-10-29）：素材为 `raw/listening/` 中的 6 段（人声、器乐、DJ、粤语、古风、邓丽君），每段有源 FLAC，以及原生、fdk、fdk ab0 在 128k/64k 下的 ADTS，同时提供了具名版和盲听版（`~/aac-listening/`）。用户反馈**听不出区别**。这是单人、非正式的试听，没有做统计意义上的 ABX，也没有覆盖更低码率或专业监听条件；只能说明在生产码率下三者没有明显的可闻差异。
- 长时间运行与真实播放：见第 4 节的 20 分钟 fdk 浸泡测试和 decode probe。

### 3.3 音画偏差与编码器延迟

用 avsync 夹具做 `--av-probe 40` 测量，四种输出都看 min/p50/max（`raw/runs/av2-*`）：

| 输入 | e1（修正前） | t1（扣除 initial_padding） | 变化 |
| --- | --- | --- | --- |
| RTMP 直通 | 0.0 | 0.0 | — |
| WHIP（Opus→AAC） | −25.8 | −4.7 | +21.1 ms，等于 AAC 1024 样本 |
| WHEP Pull（上游 AAC→Opus，本端 Opus→AAC） | −50.8 | −34.2 | +16.6 ms |

- 原因：转码器输出的 pts 从首个输入 pts 开始，按编码样本数累加，没有扣除编码器的 priming。FLV/RTMP/RTSP/HLS 播放器不会裁掉 priming，所以音频整体滞后一个编码器延迟。修正（`d159efd`）按编码器实际报告的 `initial_padding` 提前输出时间线（原生 AAC 1024、fdk 2048、libopus 312），补偿后 pts 为负的帧直接丢弃，不用任何经验常数。所有输出 min=max，没有漂移；F1 的首个 SR 对齐和 RTMP 直通都没有变化。
- 此前观察到 fdk 为 −77 ms、FFmpeg 为 −51 ms，二者相差约 26 ms。其中 21.3 ms 来自 fdk 多出的 1024 样本延迟，修正后这部分会被扣除。
- WHEP Pull 修正后仍有约 −34 ms，测量值在历次运行间有几毫秒波动（−56、−51、−50.8）。根因在上游的 SR 语义：ireader `rtcp_sr_pack` 生成 SR 时，RTP 时间戳取最后一个已发送包的时间戳加上之后经过的墙钟时间（[`rtcp-sr.c:86-89`](../../../third/ireader/librtp/source/rtcp-sr.c)、`rtp.c:70`），也就是按发送时刻映射。上游 egress 转码要攒够 960 样本才能出一个 Opus 包，音频发送因此晚于对应媒体时间；本端按首个 SR 对齐时，这段延迟就变成了音画偏差，具体数值还取决于首个 SR 前最后发出的是哪个包。同理，上游在 pts 上扣除的 6.5 ms 会被它自己的 SR 抵消，所以 Pull 的改善小于 21.3 + 6.5 ms。这是上游发送端的属性，本端接收侧无法区分，也没有修改 `third/`。浏览器等按采集时刻生成 SR 的发送端不受影响。

### 3.4 许可与发行（一手资料）

| 问题 | 资料 | 结论 |
| --- | --- | --- |
| FFmpeg 与 fdk | FFmpeg 9.0.1 `LICENSE.md`：“The Fraunhofer FDK AAC and OpenSSL libraries are under licenses which are incompatible with the GPLv2 and v3. To the best of our knowledge, they are compatible with the LGPL.” configure 把 `libfdk_aac` 列在 `EXTERNAL_LIBRARY_NONFREE_LIST` 中，只在 `--enable-gpl` 时拒绝 | GPL 构建 + fdk 不可发行；LGPL 构建 + fdk 是 FFmpeg 认为“据其所知兼容”的组合，不是法律结论 |
| 现有 SDK | `~/ffmpeg901` 为 `--enable-gpl --enable-version3`，含 x264/x265，license 为 GPLv3 | **现有部署包整体受 GPLv3 约束**，与 fdk 组合不可发行。改用最小 LGPL SDK 后，服务端不再依赖 GPL 组件 |
| fdk 许可 | fdk-aac 2.0.3 `NOTICE` | 允许免费再分发；二进制分发必须附带许可全文，并**免费提供 FDK 源码**；修改版必须更名并注明修改；不得收取版权许可费；第 3 节明确**不授予任何专利许可** |
| FSF 定性 | gnu.org license-list `#fdk` | 自由软件许可，与任何版本的 GPL 都不兼容；特别警示其不授予专利许可 |
| FFmpeg LGPL 发行 | ffmpeg.org/legal.html 合规清单 | 要求不带 `--enable-gpl/--enable-nonfree`，**使用动态链接**，随附与二进制完全对应的源码和构建说明，并在 EULA/关于页注明 |
| 静态链接 | 同上 | 当前 SDK 和运行包都是**静态链接** FFmpeg。按 LGPL 2.1 §6，静态链接要求提供可重新链接的目标文件，不符合 FFmpeg 清单的推荐做法；发行前要改为动态链接或提供可重链接材料 |
| AAC 专利 | Via LA AAC 费用页 | 许可对象是“end-user encoder and/or decoder products”，按编/解码器销售计费；分发 AAC 码流本身不收费。页面没有给出服务端软件的条款，也没有说明专利是否到期 |
| 项目自身 | 仓库根目录没有 LICENSE 文件 | 项目许可未声明；`third/` 中 ada 为 MIT/Apache 双许可，ireader 为 MIT，libsrtp 为 BSD |
| 平台 | — | 以上条款与平台无关。Windows 上的“动态链接”指 DLL；macOS/Windows 的构建本次没有验证 |

分级：

- **技术上已验证**：fdk 编码、时间线修正、生命周期测试、容量（第 4 节）。
- **工程上可维护**：固定版本的源码 + sha256 + 构建脚本 + 编译期开关，不修改 `third/`。
- **许可上可发行**：原生 AAC + LGPL 最小 SDK 在改为动态链接或提供可重链接材料后可以发行。fdk 版本在满足 NOTICE 的条件下版权层面可以发行，但**不能默认放进公开发行包**。
- **尚需外部确认**：fdk 与本项目分发方式的法律意见、AAC 专利许可是否覆盖服务端转码、项目自身的许可。

## 5. WHEP 播放侧：会话和共享转码器的放置（`82d0247`）

播放侧与 Pull 输入侧分开报告。负载为 RTMP 输入（AAC）→ 每个源 1 个或多个 WHEP 观看者，egress 共享 AAC→Opus 转码。

| 轮次 | 结果 | 实时比 min | 滞后 p50/max ms | 每源 Mcyc/s | 最忙 CPU | 控制 p99 ms | PSS MiB | FD |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| A s400w400（顺序） | PASS | 0.999 | 52/76 | 44.6 | 0.97 | 18.9 | 629 | 833 |
| A s400w400 交错 seed 11 | **FAIL** | 0.866 | 350/8316 | 42.4 | 0.99 | 607.9 | 639 | 834 |
| A s400w400 交错 seed 12 | **FAIL**（准备阶段 DTLS 超时） | — | — | — | — | — | — | — |
| B s400w400 交错 seed 11 | PASS | 0.999 | 58/78 | 44.5 | 0.97 | 23.8 | 640 | 833 |
| B s400w400 交错 seed 12 | PASS | 0.999 | 61/78 | 44.5 | 0.97 | 25.5 | 646 | 833 |
| A s50w2000 交错 seed 11 | PASS | 0.999 | 66/143 | 256.3 | 0.77 | 28.8 | 475 | 2083 |
| B s50w2000 交错 seed 11 | PASS | 0.999 | 66/88 | 261.4 | 0.78 | 19.4 | 485 | 2083 |

- A 为 HEAD `415e8aa`，B 为 `82d0247`。`--viewer-interleave-seed` 在观看者建连期间穿插控制请求，模拟生产中的混合流量。A 在交错时轮转分配会失衡：最忙的 4 个线程 1.00，最闲 0.77。B 各线程都是 0.97。
- 播放侧容量约为 400 个单观看者 AAC 源（每源约 44.5 Mcyc/s，主要是 AAC 解码 + Opus 编码）。
- 生命周期测试（180 会话、删除、churn、非法请求、源结束、优雅 TERM）在 Release/ASan/TSan/UBSan 下都通过。

## 4. 重新 profiling 与容量突破

### 4.1 fdk 版热点（280 路混合负载，perf record 15 s，`prof/` 数据见测试机）

| 类别 | fk 280 路 | fp 176 路 |
| --- | --- | --- |
| AAC 编码 | 66.2%（fdk） | 73.5%（原生） |
| Opus 解码（原生） | 11.4% | 7.1% |
| 内核（UDP 收发等） | 3.1% | 2.0% |
| libc（memmove/memset） | 2.7% | 2.0% |
| RTP/H.264/FLV | 2.3% | 1.5% |
| SRTP | 0.5% | 0.3% |
| 分配器 | 0.3% | 0.2% |

- 在 fdk 编码内部，`quantizeLines`、`calcSfbDist`、`invQuantizeLines` 合计 30.5%，基本都是 afterburner 的迭代量化。
- worker 分布：fk 在 320 路时 8 个 CPU 平均 0.971、最忙 0.975，已经完全均衡，再改均衡算法不会有收益。突破只能靠降低单路成本。
- Opus 解码约 6.4 Mcyc/s。上一轮测过 libopus 解码更慢；SRTP、分配、系统调用加起来不到 7%，没有值得做的项目。

### 4.2 fdk 容量（gk/fk，u64）

| 轮次 | 结果 | 最忙 CPU | 滞后 p50/max ms | 每路 Mcyc/s | 控制 p99 ms | PSS MiB | FD |
| --- | --- | --- | --- | --- | --- | --- | --- |
| mix280 s11 | PASS | 0.85 | 64/118 | 56.6 | 16.2 | 403 | 593 |
| seq300 | PASS | 0.91 | 68/143 | 56.3 | 15.4 | 420 | 633 |
| mix300 s12 | PASS | 0.91 | 69/201 | 56.4 | 15.7 | 426 | 633 |
| mix320 s11（fk / gk） | PASS / PASS | 0.98 / 0.97 | 132/394、105/204 | 56.3 | 21.1、18.3 | 451、455 | 673 |
| mix336 s11（gk） | FAIL | 1.00，丢包 34 万 | 866/1152 | 55.0 | 47.4 | 551 | 705 |
| 280 路浸泡 20 min（decode probe） | PASS | 0.85 | 82/215 | 56.6 | 16.4 | 403→417（5 min 后持平） | 593 |

### 4.3 afterburner=0（实验时使用 `tools/experiments/fdk-afterburner0.patch`，后作为 fdk 构建的固定行为提交为 `2a06998`）

| 轮次 | 结果 | 最忙 CPU | 滞后 p50/max ms | 每路 Mcyc/s | 控制 p99 ms | PSS MiB | FD |
| --- | --- | --- | --- | --- | --- | --- | --- |
| seq300（与 fk seq300 同时段） | PASS | 0.73（fk 0.91） | 63/80 | 35.8（fk 56.2） | 11.5 | 417 | 633 |
| mix400 s11 | PASS | 0.75 | 63/81 | 34.4 | 10.8 | 551 | 833 |
| mix440 s11 | PASS | 0.81 | 63/126 | 34.3 | 12.3 | 604 | 913 |
| mix480 s11 | PASS | 0.88 | 65/150 | 34.1 | 14.1 | 656 | 993 |
| seq496（decode probe） | PASS | 0.90 | 69/180 | 34.0 | 15.0 | 667 | 1025 |
| mix496 s12（decode probe） | PASS | 0.91 | 67/140 | 34.3 | 18.4 | 676 | 1025 |
| mix520 s11（fk0 / gk0） | PASS / PASS | 0.96 / 0.96 | 88/227、94/238 | 34.3 | 19.7、18.2 | 707 | 1073 |
| mix536 s11（fk0 / gk0） | FAIL / FAIL | 1.00，丢包 29–31 万 | 849/1189 | 34.3 | 32.5 | 805–832 | 1105 |

- 质量（离线，19 段素材）：生产档位 48k-2-128k 下 SNR 为 24.09 dB（ab1 26.71、原生 20.64），LSD 4.74（ab1 4.71、原生 5.61），编码 3.6 ms/s（ab1 9.8、原生 19.0）；单声道 64k 为 20.72 dB（ab1 22.51、原生 14.85）；48k-1-32k 为 8.71 dB，与原生的 8.70 持平。
- 带 avsync 夹具的负载轮（480 路，flv:477 + rtmp/rtsp/hls 各 1，`j-fk0-av-mix480`）：四种输出 A/V 都恒定在 −27 到 −29 ms，fps 29.98，无 PTS 回退，解码无告警。HLS 有 18 次到达停顿，与基线相同。

### 4.4 资源与观看

- 各版本 160 路时，PSS 分别为：原生 336–339 MiB，fdk 240 MiB（推测是原生 AAC 编码器每个上下文的状态更大，未单独测量），ab0 238–241 MiB。FD 都是 352–353，即每路 2 个，加上固定开销约 33。服务端线程 16 个，整个测试过程不变。
- 所有 PASS 轮均满足：服务端 UDP 丢包 0、实时比 ≥0.999、首帧 p50 约 1.0 s / p99 约 2.0 s、清理无残留。
- 临界负载附近的真实观看：fdk 280 路 20 分钟、ab0 496 路两轮，decode probe 均正常；ab0 480 路带 A/V probe 的四种输出也都正常解码。
- 生命周期（本地，最终代码 `29a7d82`）：WHEP 播放生命周期（`final-play-*`）和 WHEP Pull 生命周期（`final-pull-*`）在 ASan/UBSan/TSan 下都通过，报告中只有已知的 ASan swapcontext 提示。fdk Release 版的 pull 生命周期 `pull-fdk-rel-r2` 也通过。

## 6. 拒绝或作废的实验

| 实验 | 结果 | 处理 |
| --- | --- | --- |
| 继续优化原生 AAC（打补丁后） | twoloop 热点分布平坦，最热的一条指令 2.66%，保持逐字节一致的前提下没有低成本的收益 | 停止 |
| 调整 worker 均衡算法 | 320 路时各 CPU 平均 0.971、最忙 0.975 | 不做 |
| 720 任务（e1/fu/fp seq168 等） | `--upstream-bin` 直接指向 rt-e1 二进制，继承了被测包的库路径，上游没有启动（fu/fp 全部作废）；e1 seq168 r1 是真实的实时滞后失败 | 作废，改用 `rt-e1/upstream.sh` 包装后重测（740/800/810） |
| fk WHIP A/V 首轮 | 最小 SDK 没有 H.264 解码器，bench whip_publisher 拿不到 DTS，拒绝了夹具 | 只影响 bench；改用 rt-e1 客户端重测，通过 |
| 编译器导致约 4% 差异的推测 | 打补丁后 gcc 16.2 与 16.0.1 结果相同 | 文档中相关说法已删除，只保留可指定编译器 |

## 7. 剩余瓶颈与后续方向

- 正式版和补丁版的瓶颈都是单 worker 上同步的 AAC 编码，原生编码器占 74%。
- fdk 版：AAC 编码仍占 66%，Opus 解码占 11%，CPU 已完全均衡。再往上只能继续降低编码成本，比如 afterburner=0，或对单声道源编码单声道。后者见上一轮报告：需要可靠判断声道数，否则会把立体声误降为单声道。
- WHEP Pull 的残余音画偏差（约 −30 ms，有几毫秒波动）来自上游按发送时刻生成的 SR。如果上游也是 media_server，可以在播放侧按媒体时间生成 SR 来消除，但这要改 ireader 或在其外层自己构造 SR，属于单独任务。
- 发行方面：
  - 改为动态链接 FFmpeg，或提供可重链接材料；
  - 补齐 LGPL 与 FDK 的随附源码和声明；
  - 对 fdk 与 AAC 专利取得法律意见；
  - 声明项目自身的许可。
- 主观听感：初步试听无差异。如果要把 ab0 作为正式档位，可以再请多人做正式 ABX，尤其是 64k 以下的低码率。
- 测试机上的运行包 rt-*（5.5 GB）和本地的 build-ff*/build-g* 实验构建目录（8.5 GB）都还保留，便于复测，可按需清理。

## 提交

FINAL_HEAD `24c80ec`（main，未推送）。工作区内没有本任务未提交的代码；报告目录 `docs/performance_reports/20261029-aac-whep-capacity/` 按惯例不纳入版本控制。

| 提交 | 内容 |
| --- | --- |
| `69e8445` | WHEP 拉流 idle timer 在所属 worker 上启动 |
| `2e7bf0e` | 性能工具支持观看建连期间交错控制请求 |
| `82d0247` | WHEP 播放会话和共享音频转码器放到负载最少的 worker |
| `640497f` | 新增服务端最小 FFmpeg SDK 的可复现构建 |
| `6813359` | 性能工具支持固定上游二进制 |
| `d159efd` | 转码输出时间线扣除编码器延迟 |
| `29a7d82` | AAC 编码器改为编译期选择 |
| `abb90ef` | FFmpeg SDK 构建支持指定编译器 |
| `2a06998` | fdk AAC 编码关闭 afterburner |
| `24c80ec` | AAC 编码固定使用 libfdk-aac |

后续决定（用户，2026-10-29）：内部部署、闭源、不对外分发，因此不触发 LGPL/FDK 的分发义务。`24c80ec` 去掉编译期开关，固定使用关闭 afterburner 的 libfdk_aac，并删除 pow43 补丁（原生编码器已不再使用）。最终 SDK `~/ffmpeg-ms/9.0.1-fdk`（gcc-16）上 WHEP Pull 与 WHEP 播放生命周期测试均通过（`san/runs/pull-final-fdk-r1`、`play-final-fdk-r1`）。
