# WHEP 音频探针误报定位与修复（2026-11-05）

基线 `830668b`，只修改测试工具 `bench/whep_dump_av.py`，生产代码与 `bench/av_probe.py` 不变。

**结论：WHEP 工具修复 GO。** 80 ms 的“音频缺口”是 ffmpeg 录制端的时间轴伪影，服务端输出的 RTP 是连续的。修复后音频连续性改按 dump 中的 RTP 判定，录制侧的数值保留为 `recorded_audio_pts_*`，真实的缺口、重叠和丢包仍然报告。

## 1. 现象

长稳运行（[20261104-long-stability](../20261104-long-stability/report.md)）中，WHEP 探针在 D、B、C1 各报告 1 次 `audio_pts_gaps`，最大 80 ms。专项复现 `s3-whep-gap-r1..r3`（同一配置 150 s）中 r2、r3 报告 80 ms，r1 为 0。

`perf_run.py` 判定 WHEP 探针 `ok` 时不使用音频缺口字段，因此这些运行的 PASS 不受影响；受影响的只是报告中的字段可信度。

## 2. 证据（同一批 dump：测试机 `runs11/s3-whep-gap-r1..r3/run/whep-0.dump`，已删除，见第 7 节）

| 检查项 | 结果 |
| --- | --- |
| RTP sequence | 三份 dump 的音频与视频序号全部连续（`dumpseq.py`） |
| Opus RTP timestamp | 探针回放的前 25 s 内，每包步长都是 960（20 ms，与 TOC 一致）。整个 dump 中唯一的不规则在 92 s 处：1968、1008、912，合计多出 1008 样本，三轮位置相同，与 WHIP 测试推流端循环 120.054 s 素材时的尾部空隙一致 [推断]，不在探针窗口内 |
| RTCP SR | 视频 68 个、音频 70 个 SR；相邻 SR 之间 NTP 与 RTP 的偏差 ≤ 0.001 ms，映射呈线性。SR 所表示的音视频位置差与到达时间差之间，三轮都恒定为 61.2–61.6 ms，属于负载素材自身的属性，不用来判断音画同步 |
| 录制前后首帧 PTS | r2 运行期间的录制：视频 0, 0.033, 0.067…，音频 **0, 0.100**, 0.120…；同一份 dump 离线回放：视频 0, 0, 0.034…，音频 0.047, 0.067, 0.087…。运行期间的录制只有第一个音频包早了 80 ms（应在约 0.080，实际被放在 0），从第二个包起与离线一致 |
| 第 1 与第 2 个音频帧的间隔 | dump 中两包 RTP 时间戳相差 960，到达间隔 20 ms；录制中两包相差 100 ms。人为间隔只出现在录制中 |
| 离线回放是否可复现 | 把 r1、r2 的 dump 原样离线回放 6 次，都是 0 |
| 录制端缺包 | 把录制端的启动等待从 1.5 s 改为 0 s（诊断副本，未入库）：开头的视频丢失（690/750 帧），但音频缺口仍为 0。排除“录制端启动慢而漏包”的假设 |

## 3. 机制（确定性复现）

在每份 dump 中，第一个音频 RTP 包与第一个音频 RTCP SR 只相隔约 20 µs（r2 为 22.9 µs），回放时分别发往 RTP 和 RTCP 两个 socket，ffmpeg 读取两者的先后顺序不确定。

- 若先读到 RTP，ffmpeg 按尚未同步的时钟映射第一帧；SR 到达后，后续各帧改按 SR 的时钟映射，第一帧与第二帧之间就出现 80 ms 的人为间隔。
- 若先读到 SR，就不出现间隔。

| 变体（基于 r2 dump，只移动这一个 SR） | 回放 3 次的结果 |
| --- | --- |
| SR 移到首个音频 RTP 之后 50 ms | 3/3 报告 80 ms |
| SR 移到首个音频 RTP 之前 1 ms | 3/3 报告 0 |

服务端在发出首个 RTP 包后立即发 SR，符合 RFC 3550，不是服务端缺陷。

## 4. 修复

`bench/whep_dump_av.py`：

- **音频连续性**：按扩展序号排序 dump 中回放窗口内的音频 RTP，逐包比较“下一包时间戳”与“本包时间戳 + 本包样本数”。
  - 样本数：Opus 按 TOC 的 config 与帧数计算，G.711 按负载字节数计算；解析时处理 CSRC、头扩展和 padding。
  - 阈值沿用 `av_probe.AUDIO_GAP_SECONDS`（5 ms），没有放宽。
  - 输出 `audio_pts_gaps`、`audio_pts_overlaps`、`audio_pts_gap_max_ms`，新增 `audio_rtp_lost`（序号缺口数）。
- **录制侧数值**：保留为 `recorded_audio_pts_gaps/overlaps/gap_max_ms`，与已有的 `video_pts_backward` / `recorded_video_pts_backward` 做法一致。
- 文档字符串写明判定依据和原因。

没有采用的做法：

- 跳过第一个音频间隔：会掩盖真实发生在开头的缺口；
- 在回放中把 SR 移到 RTP 之前：会改动被测证据；
- 放宽阈值。

## 5. 验证

[`whep_dump_av_check.py`](whep_dump_av_check.py) 以一份真实 dump 为源，生成 6 个变体，再通过 `whep_dump_av.py` 实际回放到 ffmpeg 进行判定：

| 变体 | 期望 | 修复前（HEAD） | 修复后 |
| --- | --- | --- | --- |
| 原样 | gaps 0、overlaps 0、lost 0 | gaps 0，无 lost 字段 | ok |
| SR 在首个音频 RTP 后 50 ms | gaps 0 | **gaps 1（80 ms）** | ok；`recorded_audio_pts_gaps` 1 |
| SR 在首个音频 RTP 前 1 ms | gaps 0 | gaps 0 | ok |
| 第 10 s 起删除 4 个音频包 | gaps 1（80 ms）、lost 4 | gaps 1 | ok |
| 第 10 s 起时间戳 +3840（+80 ms） | gaps 1（80 ms）、lost 0 | gaps 1 | ok |
| 第 10 s 起时间戳 −960（−20 ms） | overlaps 1 | overlaps 1 | ok |

- r1、r2 两份 dump 修复后均为 PASS（6/6）。
- 修复前每个用例都因缺少 `audio_rtp_lost` 而 FAIL，其中确定性的 RED 是第 2 行的误报。
- 检查程序初版的变体函数共享了包缓冲，曾把 +80 ms 与 −20 ms 两个时间戳步进叠加成 +60 ms；修正为逐包复制后才得出上表，表中只列修正后的结果。

本地单元检查（合成包）：

- PCMA 序号回绕、头扩展和 padding：连续；
- PCMU 10 ms 重叠：overlaps 1；
- PCMA 丢 1 包：gaps 1、lost 1；
- Opus code 1 的 2×20 ms 包：连续；
- SILK 60 ms：连续；
- code 3 的 4 帧包：按 80 ms 计。

现场定向回归 `s6-whep-probe-r1..r3`：用修复后的工具副本（`rt-r` 运行包未改动）运行与 s3 相同的配置，150 s × 3 轮。结果全部 PASS HEALTHY，`audio_pts_gaps` 0、`audio_rtp_lost` 0，`ok` 均为 true。这 3 轮恰好没有触发录制伪影（`recorded_audio_pts_gaps` 也是 0），符合它的偶发性。

没有重复 35 场景协议矩阵、容量搜索或全量 Sanitizer：生产代码未变，`whep_dump_av.py` 只由 `perf_run.py` 的 WHEP 探针调用。

## 6. 仍未解决

- `recorded_audio_pts_*` 仍会偶发出现这一伪影。它现在只作为参考值，不再作为判定依据。
- A/V 偏移（`av_offset_ms_*`）仍取自录制，依赖 SR 同步之后的映射；首帧的伪影不影响之后的标记点。负载素材没有 A/V 标记，这一项在本专项中没有覆盖。
- 运行期间的录制为什么时而先读 RTP、时而先读 SR，只推断为负载下的调度差异，没有用追踪确认。确定性复现已经说明了后果，无需继续追查。

## 7. 文件

- `whep_dump_av_check.py`：基于真实 dump 的确定性回归，用例见第 8 节。用法：`whep_dump_av_check.py <whep-0.dump> --tool bench/whep_dump_av.py --ffmpeg-bin <ffmpeg> [--legacy]`。
- `whep_timeline_check.py`：只用合成 RTP 包检查判定逻辑，不需要 dump 或 ffmpeg，CI 运行它。
- `jobs/p01-whep-probe.sh`：第一轮的现场定向回归。
- **证据位置更正**：`runs11/s3-*` 的 dump 与录制、`runs11/s5-probe/` 中的变体，已在 2026-11-05 09:24 被删除。删除者是 `p01` 作业：它引用的旧作业库启动时会运行 `cleanup.sh runs8`，删掉 `runs*` 下除 `runs8` 以外所有大于 1 MB 的媒体文件。
  - 第 2、3、5 节的数值是删除之前测得的，原始文件已经无法复核。
  - 现存的 dump 只有 `s6-whep-probe-r1..r3` 三份，已复制到测试机 `evidence-whep/s6-r{1,2,3}.dump`（不在 `runs*` 下）。第 8 节的检查都以 `s6-r1.dump` 为源。

## 8. 验收判据（第二轮，2026-11-05）

第一轮只修正了音频字段的取值，`perf_run.py` 判定 `av_probe.whep.ok` 时仍然只看录制退出码、视频帧数和视频时间戳顺序。因此真实的音频丢包、缺口、重叠，乃至整条音轨缺失，都不会让运行失败。

**场景前提（核对源码）**

- 只有 `whep` 组的第一个观众录制 dump。`perf_run.py` 只给 `whep_video` 组加 `--video-only`，所以录制 dump 的这个观众在 offer 中总是请求音频。
- `whep_fanout` 自身的就绪条件 `expects_audio()` 也要求收到音频包。
- 自有源模式下，素材必须同时含音频和视频，否则运行判为 UNSUPPORTED。

据此 runner 给 `whep_dump_av.py` 传 `--expect-audio`。纯视频 offer（`--video-only`）或 G.722-only offer（`--unsupported-audio`，服务端拒绝音频）的观众不录制 dump；工具在不带 `--expect-audio` 时，不要求存在音轨。

**判定**（`whep_dump_av.py` 输出 `checks`、`verdict`、`ok`；FAIL 时退出码为 1）

| check | 条件 |
| --- | --- |
| `recording` | 回放录制和 av_probe 成功（与旧判据相同） |
| `video_decoded` | 视频帧数 > 0 |
| `video_pts_order` | dump 中的视频 RTP 时间戳没有超过 0.5 s 的回退 |
| `audio_present` | 带 `--expect-audio` 时，必须有音频 RTP |
| `audio_timeline` | 有音频时：`audio_rtp_lost`、`audio_pts_gaps`、`audio_pts_overlaps` 都为 0，阈值仍是 5 ms；可判定的包少于 2 个时为 null |

- verdict：任一 check 为 false 时是 FAIL；没有 false 但有 null 时是 INCONCLUSIVE；其余为 PASS。
- `perf_run.py` 的处理：`ok = 退出码 0 且 verdict == PASS`。INCONCLUSIVE 不算失败，但整次运行最多只能是 INCONCLUSIVE/UNCONFIRMED，不会是 PASS。

**合法间断**

推流端用 `-stream_loop -1`，或由 `whip_publisher` 循环播放 120 s 素材。循环处会出现输入侧的音频间断，例如上次 WHIP 素材在约 92 s 处多出 21 ms。runner 按下面的方法计算源保证尚未循环的上界：

- 取最早一个推流进程的 starttick（换算到 monotonic）；
- 加上素材时长（`--fixture` 与 `--whip-fixture` 取较短的一个）；
- 减去全部观众就绪的时刻（dump 中首包一定早于这个时刻）；
- 结果作为 `--source-continuous-ns` 传给工具，记录在 `av_probe.whep.source_continuous_seconds`。

只判定这个上界之前的音频包。外部源（`--input existing`）中有不带 `publisher_pid` 的源时，上界取 0，因此结论是 INCONCLUSIVE。

**RED→GREEN**（源为 `evidence-whep/s6-r1.dump`，每个用例都实际回放到 ffmpeg）

| 用例 | 期望 | 旧判据（HEAD 工具，`--legacy`） | 新工具 |
| --- | --- | --- | --- |
| 原样 Opus | PASS | PASS | PASS |
| SR 在首个音频 RTP 后 50 ms | PASS | PASS | PASS（`recorded_audio_pts_gaps` 1） |
| SR 在首个音频 RTP 前 1 ms | PASS | PASS | PASS |
| 第 10 s 起删除 4 个音频包 | FAIL | **PASS（漏报）** | FAIL，lost 4，80 ms |
| 时间戳 +80 ms | FAIL | **PASS（漏报）** | FAIL，gap 80 ms |
| 时间戳 −20 ms | FAIL | **PASS（漏报）** | FAIL，overlap 1 |
| 序号与时间戳回绕 | PASS | PASS | PASS |
| 回绕后删除 4 包 | FAIL | **PASS（漏报）** | FAIL，lost 4 |
| G.711 A-law（由同一 dump 改写为 PT 8、8 kHz、160 字节/包） | PASS | PASS | PASS |
| G.711 删除 4 包 | FAIL | **PASS（漏报）** | FAIL，lost 4 |
| 期望音频但没有音频 RTP | FAIL | **PASS（漏报）** | FAIL，`audio_present` |
| 纯视频或音频被拒绝（无音轨，不带 `--expect-audio`） | PASS | PASS | PASS |
| +80 ms 跳变位于前提上界（5 s）之后 | PASS | PASS | PASS（判定 224 包） |
| 前提上界为 0 | INCONCLUSIVE | **PASS** | INCONCLUSIVE |

- 旧判据 14 个用例中 7 个与期望不符；新工具 14/14 全部符合。
- `whep_timeline_check.py` 的 12 个合成用例：新工具全部通过；HEAD 版工具只有 `no-audio` 一例失败（旧实现不输出 `audio_timeline_packets`），其余 11 例的时间线计算两者一致。

**现场回归**：见 [20261105-rc-readiness](../20261105-rc-readiness/report.md) 中用 RC 二进制运行的完整协议矩阵。
