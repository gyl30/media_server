# RTSP AAC 时间戳锯齿：最小修复可行性（2026-11-04）

基线 `830668b`。**结论：KEEP / NO-GO。** 不修改生产代码，保留协议矩阵中 3 个 RTSP AAC 场景的严格 XFAIL。

## 结论

- 锯齿可以确定性复现。只要一个 RTP 包携带多个 AAC AU，RTSP 推流和 RTSP Pull 两条接收路径产生的偏差完全一致：48 kHz 下为 0～−10.667 ms，44.1 kHz 下为 0～−7.882 ms；每个包只带 1 个 AU 时，误差只有 1 ms 以内的毫秒量化。
- 现有库接口无法提供修复所需的信息。
  - ireader 的 RFC 3640 解包器给包内每个 AU 都回调同一个 RTP 时间戳，不提供 AU 在包内的序号。
  - 丢包后，它会丢掉下一个包的第一个 AU。
  - AAC 解析结构不提供 frameLengthFlag（960 或 1024）。
  - 上游 master 的这部分行为与本地相同。
- 在 `third/` 之外能够正确修复的方案有三类，都需要自己维护第二套 RTP→PTS 时间线，或者重新解析 RTP 负载；按任务约束，全部 REJECT。

## 1. 已知证据（第一阶段）

[20261031 报告阶段 C](../../performance_reports/20261031-gb-ps-rtsp-aac-chrome-opus/report.md) 已定位到以下几点：

- FFmpeg 推流时，每个 RTP 包携带 33 个 AU，RTP 时间戳本身正确；
- `rtp-mpeg4-generic-unpack.c` 让包内每个 AU 都使用包的时间戳；
- `avpbs-aac.c` 遇到不大于上一帧的 PTS 时，按整数毫秒 `1000*1024/sample_rate` 递增；
- 解码样本数和逐帧内容都完整，问题只出在时间戳上。

本次没有重复做性能和全协议分析。

生产代码核对结果：

- `rtsp_publish_media` 和 `rtsp_pull_media` 都把 RTP 包整包交给 `rtsp_demuxer_input`，在回调中把 `avpacket_t::pts`（毫秒）转换为纳秒；
- RTSP TCP 和 UDP 推流都进入同一个 `rtsp_publish_media::input_packet`，两种传输只负责分帧；
- RTP→PTS 映射（64 位扩展、`base_pts`、RTCP SR 对齐、Pull 的 RTP-Info）全部在 ireader 的 `rtsp-demuxer.c` 内部结构 `rtp_payload_info_t` 中完成，外部不可见。

## 2. 确定性回归（第二阶段）

[`rtsp_aac_timeline_check.cpp`](rtsp_aac_timeline_check.cpp)：

- **运行方式**：直接驱动生产代码 `rtsp_publish_media` 和 `rtsp_pull_media`，经 `stream_registry` 订阅 `media_stream` 并收集已发布的帧。轨道描述由生产代码 `rtsp_sdp_track_from_format` 生成。
- **构造的输入**：AAC-hbr RTP 包（13/3/3 位 AU 头），时间戳已知；每个 AU 的负载前 4 字节写入全局 AU 序号，用来核对内容和顺序。
- **期望 PTS**：（AU 所在包的 RTP 时间戳 − 起点）+ 包内序号 × 1024，在 RTP 时钟单位中计算后再换算为毫秒。
- **判据**：帧数与发送数一致；内容序号一致；每帧 |PTS 误差| ≤ 1 ms（容许现有毫秒级时间线的量化误差）。

编译方式：`../20261104-library-review/build_check.sh build-rel rtsp_aac_timeline_check.cpp <out>`；设置 `CHECK_VERBOSE=1` 时会列出丢失的 AU。

当前实现（Release）的结果如下。publish 与 pull 的数值完全相同，表中只列一次；P/U 分别表示 publish、pull。

| 场景 | 帧数 | PTS 误差 ms | 相邻步长偏差最大 ms | 结果 P/U |
| --- | --- | --- | --- | --- |
| 48k，1 AU/包 | 400/400 | −0.667～0 | 0.667 | ok/ok |
| 48k，33 AU/包 | 396/396 | **−10.667**～0 | 10.667 | **FAIL/FAIL** |
| 44.1k，1 AU/包 | 400/400 | −0.998～0 | 0.780 | ok/ok |
| 44.1k，33 AU/包 | 396/396 | **−7.882**～0 | 7.780 | **FAIL/FAIL** |
| 48k，1 AU/包，时间戳回绕 | 400/400 | −0.667～0 | 0.667 | ok/ok |
| 48k，33 AU/包，时间戳回绕 | 396/396 | −10.667～0 | 10.667 | FAIL/FAIL |
| 48k，1 AU/包，丢包 50、51、200 | **395/397** | −0.667～0 | 0.667 | FAIL/FAIL |
| 48k，33 AU/包，丢第 4 个包 | **362/363** | **−31.667**～0 | 31.667 | FAIL/FAIL |
| 48k，1 AU/包，时间戳前跳 1 s | 400/400 | −0.667～0 | 999.667（真实跳变） | ok/ok |
| 48k，33 AU/包，时间戳前跳 1 s | 396/396 | −10.667～0 | 1010.667 | FAIL/FAIL |
| 48k，1 AU/包，中途 RTCP SR | 400/400 | −1.000～0 / −0.667～0 | 0.667 | ok/ok |
| 48k，33 AU/包，中途 RTCP SR | 396/396 | −10.667～0 | 10.667 | FAIL/FAIL |

“相邻步长偏差”只在相邻两帧的 AU 序号连续时计算。初版检查程序只看发送序列是否连续，没有核对上一帧是否为前一个 AU，因此丢包两行曾记为 63.667 和 714.667 ms，混入了丢失的 AU 和丢失的包造成的缺口。修正这个判据后重新运行，只有这两行的这一列发生变化，帧数、PTS 误差和 FAIL 判定都没有变。

三类误差的区分：

- **量化误差**：1 AU/包时误差 ≤ 1 ms。这来自 `timestamp*1000/frequency` 的整数毫秒映射，SR 重新对齐后多出 0.333 ms，误差不累积。
- **包内锯齿（本问题）**：多 AU/包时，包内第 k 个 AU 的误差为 −k×(1024/fs − ⌊1000×1024/fs⌋ ms)，到下一个包首帧重新对齐。回绕、真实跳变和 SR 都不改变这一形态；真实跳变被原样保留，没有被错误重置。
- **丢包**：被丢的包之后，紧接着的那个完整包的第一个 AU 也被丢弃（1 AU/包时丢的是 AU 52、201，33 AU/包时丢的是 AU 165）。原因是 `rtp-payload-helper.c` 的 `rtp_payload_check` 遇到序号缺口时，无法判断丢失的包属于前一帧还是后一帧，于是把下一帧标记为损坏并丢弃。在多 AU 包里，由于第一个 AU 被丢掉，第二个 AU 拿到了包的时间戳，误差再增加一个 AU 的时长（−21.333 ms，合计 −31.667 ms）。这是既有的库行为，与锯齿同源，都缺少 AU 的包内序号。

TCP 与 UDP 推流共用 `input_packet`，因此本回归同时覆盖了两者的接收实现；两种传输的端到端行为由协议矩阵中的 `rtsp-tcp-*` 和 `rtsp-udp-*` 场景覆盖。

### 2.1 无丢包时的锯齿与丢包时的帧损失

这是两个独立的问题，处理方式也不同，不能合并计算：

| | 无丢包时的 PTS 锯齿 | 丢包时的额外 AU 损失 |
| --- | --- | --- |
| 触发条件 | 每个 RTP 包携带多个 AU（FFmpeg 推流默认就是这样），每个包都会出现 | 只在 RTP 序号出现缺口时出现，单 AU 包和多 AU 包都会发生 |
| 媒体数据 | 完整：帧数、内容、解码样本数都与素材一致 | **真实丢帧**：缺口之后第一个完整包的首个 AU 被库丢弃（每处缺口多丢 1 个 AU，48 kHz 下 21.3 ms 音频） |
| 时间戳 | 包内第 k 个 AU 偏早 k×0.333 ms（48 kHz）或 k×0.220 ms（44.1 kHz），在下一个包首帧重新对齐 | 同一包内剩余的 AU 再整体偏早 1 个 AU（48 kHz 下 −21.333 ms），与锯齿叠加，最大 −31.667 ms；从下一个包开始恢复 |
| 确定性结果 | 48k 33 AU：0～−10.667 ms；44.1k 33 AU：0～−7.882 ms；1 AU/包：≤ 1 ms | 48k 1 AU/包丢 3 个包：395/397（缺 AU 52、201）；48k 33 AU/包丢 1 个包：362/363（缺 AU 165） |
| 原因 | 解包器给包内各 AU 回调同一个时间戳，avpbs 按整数毫秒递增 | `rtp_payload_check` 遇到序号缺口时把下一帧标记为损坏；对于多 AU 包里完整的 AU，这样处理过于保守 |
| 矩阵覆盖 | 3 个严格 XFAIL：`rtsp-tcp-h264-aac`、`rtsp-udp-h264-aac`、`rtsp-tcp-h265-aac` | 矩阵在本机回环上测试，不丢包，不覆盖这一项 |

### 2.2 960 样本 AAC 帧

ireader 的 `mpeg4_aac_t` 不读取 GASpecificConfig 中的 frameLengthFlag，`avpbs-aac.c` 写死 1024 样本。`parse_aac_asc` 接受的 AOT 1–4 按标准都可以使用 960 样本帧。遇到这类源时：

- 无论单 AU 还是多 AU，库补出的毫秒步长都按 1024 样本计算，会偏长约 6.7%（48 kHz 下 21 ms 对 20 ms）；
- 多 AU 包内的时间戳会越来越偏晚，在下一个包首帧回到 RTP 时间；单 AU 包按 RTP 时间戳映射，不受影响。

本次没有 960 样本的素材或推流端可以实测，以上只是按代码推断的兼容性风险；也没有为它增加检查场景，检查程序固定使用 1024 样本。

## 3. 库接口（第三阶段）

| 需要的信息 | ireader 现状 | 依据 |
| --- | --- | --- |
| 包的原始 RTP 时间戳 | 只在内部可用。`rtsp_demuxer` 回调只给出毫秒 PTS；底层 `rtp_demuxer` 的回调能给出 32 位 RTP 时间戳 | `rtsp-demuxer.c` `rtsp_demuxer_onrtppacket`；`rtp-demuxer.h` |
| AU 在包内的序号 | **不提供**。每个 AU 单独回调，时间戳相同，没有序号；AU-Index/AU-Index-delta 被丢弃 | `rtp-mpeg4-generic-unpack.c` 循环 |
| `sizelength/indexlength/constantDuration` | SDP 解析器读取了这些参数，但解包器不使用，写死为 13/3 位 | `sdp-a-fmtp.h`；同上 |
| 每个 AU 的样本数 | **不提供**。`mpeg4_aac_t` 不含 frameLengthFlag；`avpbs-aac.c` 写死 1024。我们的 SDP 接受 AOT 1–4（Main/LC/SSR/LTP），这些类型都允许 960 样本帧 | `mpeg4-aac.h`；`codec_utils.cpp` `parse_aac_asc` |
| 丢包信息 | 只在下一帧上标记 `RTP_PAYLOAD_FLAG_PACKET_LOST`，并且丢掉该帧；无法得知丢了几个 AU | `rtp-payload-helper.c` |
| 上游是否已修复 | 没有。上游 master 的 `rtp-mpeg4-generic-unpack.c` 同样为包内每个 AU 使用同一时间戳，并写死 AU 头长度 | 公开源码 |

## 4. 候选方案（不改 `third/`）

| 方案 | 做法 | 新增代码与每连接状态 | 丢包语义 | 结论 |
| --- | --- | --- | --- | --- |
| A 从毫秒 PTS 反推 | 在回调中把 `pts == 上一帧 + ⌊1000×1024/fs⌋` 识别为“库补出的时间戳”，改用精确时长 | 约 20 行，每轨 2 个值 | 合法的 1 AU/包流的真实步长也可能恰好等于该值，二者无法区分；依赖 avpbs 的内部公式；仍然假设 1024 | **REJECT**：属于猜测 |
| B 预解析 RTP 头 | `input_packet` 先自行解析时间戳和 AU 数，回调时按顺序对应 | 约 60 行，每轨队列 | 500 ms 抖动队列会重排和延迟释放，丢包后又少一个 AU，对应关系不确定 | **REJECT**：重复解析，丢包处理不正确 |
| C AAC 轨绕过 `rtsp_demuxer` | 改用 `rtp_demuxer` 直接回调，按“相同时间戳计数”得到包内序号，再自己完成 64 位扩展、基准、RTP-Info、SR 对齐、ASC 和 ADTS | publish 与 pull 各约 120–150 行，每轨约 8 个状态，外加 frameLengthFlag 解析 | 丢包后序号偏一位，只能退回包时间戳；音频与视频的基准换成两套实现，A/V 对齐需要另行保证 | **REJECT**：等于第二套时间线状态机 |
| D 自己实现 RFC 3640 解包 | 按 fmtp 解析 AU 头，计算每个 AU 的时间戳 | 约 200 行以上 | 可以正确处理 | 任务明确禁止 |
| E 在 ireader 内修复 | 解包器按 `ts + i×帧长` 回调；avpbs 不再按毫秒补；丢包时多 AU 包不丢首帧 | 约 30–50 行 | 正确 | 需要授权修改 `third/`，本轮不做 |
| F FFmpeg RTSP | 用 FFmpeg 完整的 RTSP 协议栈替换 | 大 | 正确 | 任务明确禁止 |

CPU 和内存都不是决定因素：A–C 的开销都是每帧常数次运算。决定因素是正确性和维护成本。A 和 B 无法取得必要的信息，C 必须复制 `rtsp_demuxer` 的时间线逻辑。按任务约束，“不能为消除 10.67 ms 的锯齿引入更复杂的时间线状态机”，因此不实施。

## 5. 决定与影响

- 不改生产代码；`regression/protocol_matrix.py` 中 `RTSP_AAC_TIMESTAMP_JUMPS`（`rtsp-tcp-h264-aac`、`rtsp-udp-h264-aac`、`rtsp-tcp-h265-aac`）的严格 XFAIL 保持不变。这条规则在锯齿消失时会以 XPASS 报出，提示移除。
- 由于没有生产改动，第五阶段（Sanitizer、完整矩阵、性能）没有执行。最近一次完整验证见 [20261104-port-pool-replay](../20261104-port-pool-replay/report.md)：Release 32 PASS + 3 XFAIL。
- 现有影响沿用 20261031 阶段 C 的评估：误差上限 10.67 ms（44.1 kHz 为 7.9 ms），每包重新对齐，不累积；样本完整，低于唇音同步的可察觉范围。
- 新增记录：丢包后会额外丢掉 1 个完整 AU，多 AU 包内其余 AU 的时间戳再偏早一个 AU。这是既有库行为，本轮不改。
## 6. 未解决问题与后续方向

本报告就是这些问题的记录位置，没有另建 issue。

1. **多 AU 包的 PTS 锯齿**（KEEP，保留严格 XFAIL）。
2. **丢包后额外丢弃完整 AU**（既有库行为，矩阵没有覆盖）。
3. **960 样本帧的时长假设**（代码推断的风险，没有实测）。

如果以后允许修改第三方库，优先评估在 ireader 内做最小修复（方案 E），而不是在第一方代码里重建时间线：

- `rtp-mpeg4-generic-unpack.c`：按 fmtp 的 `sizelength/indexlength` 解析 AU 头，以“包时间戳 + 包内序号 × AU 时长”回调每个 AU；AU 时长取 `constantDuration` 或 ASC 帧长。
- 同一文件中，多 AU 包内的 AU 都是完整的，序号缺口不应导致下一个包的首个 AU 被丢弃。
- `avpbs-aac.c`：在上游给出精确时间戳之后，不再按整数毫秒补时间。
- 第一方 `rtsp_publish_media` 和 `rtsp_pull_media` 预计不需要改动；本检查程序可以直接作为 RED→GREEN 判据，修复后再按完整矩阵确认那 3 个 XFAIL 变为 XPASS，然后再移除规则。

上述修改需要单独授权修改 `third/`，并且应优先向上游提交。
