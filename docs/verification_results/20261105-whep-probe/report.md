# WHEP 音频探针误报定位与修复（2026-11-05）

基线 `830668b`，只修改测试工具 `bench/whep_dump_av.py`，生产代码与 `bench/av_probe.py` 不变。

**结论：WHEP 工具修复 GO。** 80 ms 的“音频缺口”是 ffmpeg 录制端的时间轴伪影，服务端输出的 RTP 是连续的。修复后音频连续性改按 dump 中的 RTP 判定，录制侧的数值保留为 `recorded_audio_pts_*`，真实的缺口、重叠和丢包仍然报告。

## 1. 现象

长稳运行（[20261104-long-stability](../20261104-long-stability/report.md)）中，WHEP 探针在 D、B、C1 各报告 1 次 `audio_pts_gaps`，最大 80 ms。专项复现 `s3-whep-gap-r1..r3`（同一配置 150 s）中 r2、r3 报告 80 ms，r1 为 0。

`perf_run.py` 判定 WHEP 探针 `ok` 时不使用音频缺口字段，因此这些运行的 PASS 不受影响；受影响的只是报告中的字段可信度。

## 2. 证据（同一批 dump：测试机 `runs11/s3-whep-gap-r1..r3/run/whep-0.dump`）

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

- `whep_dump_av_check.py`：确定性回归。用法：`whep_dump_av_check.py <whep-0.dump> --tool bench/whep_dump_av.py --ffmpeg-bin <ffmpeg>`。
- `jobs/p01-whep-probe.sh`：现场定向回归。
- 原始 dump、运行期间的录制和诊断输出：测试机 `runs11/s3-*`、`runs11/s5-probe/`、`runs11/s6-*`。
