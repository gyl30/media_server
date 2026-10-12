# Performance Test Plan Review

## 独立结论与适用范围

本审查以源码、原始结果和 TEST 只读拓扑为依据。已先完整阅读原主仓库的 `AGENTS.md`、`docs/project_decisions.md`、`docs/development_conventions.md`；独立 worktree 没有 `AGENTS.md`。没有运行 TEST 媒体、容量、soak 或 profiler，没有进入 Pi namespace，没有修改生产代码、系统设置或第三方代码。

审查的最终工具源码提交为 `21bba8cf837eb2f19030b2dc4b09a94694cc80db`；BASE 为 `b0b67da8a3f020f1ee8b15673d57f999042beb57`，远端基线为 `9486c8602ef3f5acdc63e4cafdb05ff6dec6c10e`。本文件的审查范围是性能工具与测试方案，不是生产容量验收。后续文档提交不会改变工具代码；实际运行包仍须按其 manifest/ELF/fixture SHA 核验。

**总体：CONDITIONALLY_READY，限于已声明的范围。** 适用于固定 H264/AAC 素材（WHIP 输入 H264/Opus 后转 AAC）、native 媒体进展、独立网络环境、固定 source 数的短窗和 viewer 搜索。本轮 P0 工具保护已有 GREEN；执行条件仍是正式预检全部成立、使用非零安全预算，并保留全部原始失败。未满足条件时不能开始正式搜索。

**完整目标：NOT_READY。** 现有工具组合尚不能独立完成严格 idle/publish-only、全部 codec 的输入输出矩阵、完整网络 Mbps/PPS/重传归因、source×viewer 双向容量面、真实浏览器解码和长时间稳定性验收。以下 PARTIAL/INSUFFICIENT 不等于生产不支持，也不等于已有短测 FAIL。

## 8 输入 × 8 输出覆盖

`existing` 是使用外部 source 的运行方式，不是第九种生产输入。以下八种输入区分实际 transport/角色；RTSP pull 和 RTSP 输出均为 TCP，不能由 UDP publish 推导出 UDP play/pull。

每格为 **生产路径 / 工具路径 / 本轮 DEV 证据**：`S` 表示标准 H264/AAC canonical 路径由当前源码支持；`T` 表示正式 runner 有可规划路径；`D` 表示已有完整短窗口 PASS；`p` 仅表示准备期 FLV 双轨 probe PASS；`N` 表示该格正式窗口 NOT_RUN。**所有格的 TEST 媒体/性能均 NOT_RUN**；运行包解包、依赖和 help 通过不改变此状态。

| 输入\输出 | RTMP | RTSP TCP | HTTP-FLV | HLS | WHEP | GB UDP | GB TCP active | GB TCP passive |
|---|---|---|---|---|---|---|---|---|
| RTMP | S/T/D | S/T/D | S/T/D | S/T/D | S/T/D | S/T/D | S/T/D | S/T/D |
| RTSP TCP publish | S/T/N | S/T/N | S/T/D | S/T/N | S/T/N | S/T/N | S/T/N | S/T/N |
| RTSP UDP publish | S/T/N | S/T/N | S/T/D | S/T/N | S/T/N | S/T/N | S/T/N | S/T/N |
| RTSP TCP pull | S/T/N | S/T/N | S/T/D | S/T/N | S/T/N | S/T/N | S/T/N | S/T/N |
| WHIP | S/T/N | S/T/N | S/T/p | S/T/N | S/T/D | S/T/N | S/T/N | S/T/N |
| GB UDP receiver | S/T/N | S/T/N | S/T/D | S/T/N | S/T/N | S/T/N | S/T/N | S/T/N |
| GB TCP active receiver | S/T/N | S/T/N | S/T/D | S/T/N | S/T/N | S/T/N | S/T/N | S/T/N |
| GB TCP passive receiver | S/T/N | S/T/N | S/T/D | S/T/N | S/T/N | S/T/N | S/T/N | S/T/N |

`S/T` 是源码支持和工具能力判断，**不是 64 格 E2E 已通过**。DEV 的十个正常场景为 `dev-integration/summary.json` 所列 8s 小负载；各次运行的 HEAD/dirty 以原始记录为准，`D`不表示64格都在最终clean提交重测。最终保护后的 RTMP 五输出 smoke 和真实 Go 授权回归另见 [verification](verification.md)。GB 输入使用 upstream media_server sender → DUT receiver，不能替代 SIP REGISTER/Catalog/INVITE、模拟设备或厂商互通。

生产 codec 能力比本矩阵宽。RTMP/RTSP 与 FLV 支持 H264/H265，以及有参数约束的 AAC/Opus/G711；GB PS 支持 H264/H265 + AAC/G711，拒绝源 Opus；HLS 跳过 Opus 音轨。因此“视频继续输出”不能视为要求的双轨组合 PASS。native WebRTC offer 只有 H264 `42e01f` 和 Opus，不能覆盖 H265、PCMA/PCMU WHEP；HLS client 不解析独立音视频轨，GB client 不解码或验证全部 PSM codec。

AAC→Opus WHEP 是 [whep_audio_egress](../../media/webrtc/whep_audio_egress.cpp) 按 source generation/参数共享的派生路径，每个 viewer 仍独立 RTP/SRTP/DTLS。WHIP 输入 [whip_media_receiver](../../media/webrtc/whip_media_receiver.cpp) 明确将 Opus→AAC 后进入 canonical source。因此 WHIP→WHEP 实际包含 Opus→AAC→Opus；不能称 Opus 直通，也不能将每 viewer 成本与每 source 转码成本混合解释。纯音频 publisher、通用视频转码、新 WebRTC 控制协议不是本轮缺口，保持历史产品边界。

## 场景充分性

| 场景 | 当前组合与本轮事实 | 评价 / 下一轮必须证明 |
|---|---|---|
| idle | perf_run 会建 source，没有真正 idle 模式 | INSUFFICIENT；独立持久 DUT 无 source/viewer 的同长度资源基线，固定服务与授权依赖 |
| publish-only | `--outputs none` 在准备期 probe 后仍运行每 source 一个 FLV viewer，覆盖整个窗口 | PARTIAL；可称 publish+FLV/source，严格零输出另组合发布者与外部资源采样，不拿它冒充纯输入成本 |
| 单 source、小 viewer | 正式 native 客户端 ready、共同 barrier、窗口覆盖、两轨增长、清理 | SUFFICIENT（限定媒体进展契约）；不证明 fps、音频采样连续性或解码 QoE |
| 多 source | `--sources`、round-robin 分配；DEV 双 source FLV10/WHEP2 | PARTIAL；输入数和输出数分别增长，核对每 source 实际消费分配与 source CPU |
| fanout / viewer 容量 | perf_capacity 递增、二分、H/F 独立复验 | SUFFICIENT（固定 source）；真实 TEST H/F 仍 NOT_RUN，不继承历史 2200 成绩 |
| mixed | perf_run 可给各协议不同数量；capacity 均分协议总量 | PARTIAL；先固定并记录实际协议/source 分布，不能把均分结果外推任意 mix |
| 双向增长 | capacity 仅搜索 viewer 轴 | INSUFFICIENT；独立固定 viewer/source 的 source 轴，以及固定 source 的 viewer 轴，再选代表性二维组合 |
| churn | 同持久 DUT、每 wave 新授权、existing windows；本轮三次小 wave | PARTIAL；需明确定义创建/删除率、波峰、重叠连接和基线回落，多协议/长时尚未证明 |
| source replacement | perf_publish cycles、新 generation、新 ticket；本轮旧消费者结束与新短窗可用 | PARTIAL；HLS 旧 playlist/retention、全部输出和多次代际隔离不能由两代小样本替代 |
| longrun / soak | 旧 lifecycle 工具授权不兼容，本轮未运行 | INSUFFICIENT；不能只把 duration 拉长，需下述长期证据与采样前置 |

推荐计划按风险递进：环境/P0 → 单 source 单 viewer 合法媒体与负对照 → 上表中覆盖不同转换/封装成本的代表性路径及必要独立解码 → idle/发布基线 → source 轴和 viewer 轴 → 明确 mix → 受控 churn/replacement → 独立诊断 → 长期场景。无需机械运行 64 格全矩阵，未执行格继续标 NOT_RUN；固定 source 的短时 viewer 测试不必等待长期采样工具补齐，但不能据此交付长期或二维容量结论。每阶段独立目录、准入与停止原因，前阶段失败不进入依赖该结果的后阶段。

稳定性计划须预先写明目标时长、每阶段 source/viewer 数、创建删除率、周期替换频率、HLS retention 后观察时间、逐窗健康判据、内存/FD/socket 回落与中止标准。本轮秒级 smoke 或历史约 40 分钟 soak 不能替代未来多小时/多日目标。perf_run 将全部 samples 留在内存并最终一次写 JSON；长时、多 group 会持续增长 runner 内存。自管 WHIP publisher 固定运行 86400s，准备与 warmup 后的 24h 测量会跨过其自然结束。正式长时前需有界/流式采样、周期保存与足够 publisher 生命周期，先验证这些工具契约；不要把工具先结束当 DUT 失败。

## 指标与归因充分性

| 必需指标 | 当前证据 | 缺口与解释边界 |
|---|---|---|
| DUT/每线程 CPU | /proc 区间 CPU seconds、各 TID、affinity；client 可用自身窗口 CPU counter | PARTIAL；区分各采样时刻与客户端正式窗口，新增/退出线程缺端点不能记零；尚无逐窗 CPU 分位数、重复汇总 |
| RSS/PSS/峰值 | 约 1Hz samples 与 sampled max、FD、VmSize | PARTIAL；sampled max 不是真峰值，VmSize 不是物理内存；需 VmHWM 或独立峰值证据、同持久 DUT retention/释放后趋势 |
| 网络 Mbps/PPS | native payload/RTP/PS bytes，WHEP/GB packet 数；GB 子结果有 rate | PARTIAL；缺 NIC/lo/veth rx/tx bytes/packets 的窗口差分，不能把 payload rate 写成 wire Mbps/PPS |
| drops/retrans/队列 | owned socket 队列/drop 快照，namespace SNMP raw，新增窗口 server 日志 | PARTIAL；socket 删除会丢历史，namespace 计数不能独占归因 DUT；需明确接口/socket/namespace 差分和共享者 |
| connection establishment rate | 配置 ramp；WHEP/GB 有总体建立秒数 | PARTIAL；不是完整的成功/失败建立率或独立服务器处理速率，授权/关键帧/客户端调度会混入 |
| first media latency | RTMP/RTSP/FLV/WHEP 每 group p50/p95/p99/max；HLS raw，GB p10/p50/p90 子 JSON | PARTIAL；统一 runner 的 HLS/GB first_media 为空，不能平均 group p99 得全局 p99；少量 viewer 无可靠尾分位含义 |
| 每 viewer/每窗口媒体 | 真实双轨计数/序号/时间戳，HLS unique segment；实际公共窗口覆盖 ≥99% | PARTIAL；增长不等于目标 fps/bitrate/解码，TCP native 的尾窗不单独检查；需原始每窗速率与低负载/fixture 对照 |
| repeat statistics | H≥3、F≥2 独立复验与原始 rounds | PARTIAL；需同条件 CPU/内存/网络/延迟的重复分布与离散度，搜索的 H/F 重复不能替代全部指标统计 |
| DUT/source/client 归属 | 新保护记录稳定 PID/starttick/exe/netns；source manifest 记录 publisher starttick | CONDITIONAL；same ELF 的另一服务、真实 URL 与监听 PID 对应关系仍须正式预检保存证据 |
| 压力停止 | 准备/测量预算、有限超时、分类与 own process 清理、capacity 调度中止 | CONDITIONAL；默认预算0只观测，正式须明确非零，PSI/SMT/其他服务影响仍需 owner 监督 |

原始 HLS/GB 子 JSON、各协议 log 是证据组成部分，不能只看 runner 总表。正式统计应明确样本对象、单位、窗口、样本数、分位算法、重复次数；特别保留失败 viewer，不能用请求总数稀释失败。首媒体包含连接/鉴权/等关键帧，不是服务器纯处理延迟。CPU counter/时间差、inclusive perf 栈、allocator allocated/resident/retained 和 PSS 各自有边界，不能相加或换算为未经验证的可省成本。

## TEST 物理拓扑与隔离实证

2026-10-12 通过 DEV SSH 只读执行 TEST `lscpu -e=CPU,CORE,SOCKET,NODE`、thread_siblings_list、`numactl --hardware` 与进程检查：单 socket、NUMA node0；CPU0–15 对应不同物理 core0–15，CPU16–31 是对应 SMT 线程。实读 cpu0 siblings=`0,16`，cpu8 siblings=`8,24`。

| 约定 affinity | 物理核 | 未纳入该 affinity 的 SMT siblings |
|---|---|---|
| server 0–7 | core0–7 | 16–23 |
| client/source 8–15 | core8–15 | 24–31 |

因此 server0–7 与 client8–15 **彼此不共享物理核**；它们仍可分别与运行于16–31的其他任务共享物理核。namespace 仅隔离网络，不隔离 CPU/NUMA/缓存/内存带宽。固定 node0 也不是独立物理资源证明。

第二次观察为 `2026-10-12T03:16:31Z`：`pgrep -x media_server` 没有返回 PID；namespace 列表包含本轮 `mstooling-codex-20261012-b496` 和 `media-test`，未进入二者。故当时 Pi DUT 的实际 PID/每线程 affinity **NOT_OBSERVED**，不以此声称机器空闲或无人使用 siblings。正式测试前重新保存目标 PID/starttick/exe/netns、每 TID affinity、`ss -lntup`/proc FD 的地址端口与 PID 归属，以及 siblings 上其他任务和背景负载；无法核验时不能报告完全物理隔离或纯 DUT 瓶颈。

client/source 使用同一8–15集合是现行约定；FFmpeg、upstream server、signaling/runner 与客户端开销需要单独归因。某 source/client thread 接近一核、FD/端口/UDP buffer 或主机预算先触界限只能 BLOCKED/INCONCLUSIVE，不能作为服务器 F。

## 工具适用矩阵

评价针对右栏限定职责，不把未实际执行等同工具不支持。

| 工具/组合 | 评价 | 可承担职责 / 缺少职责 |
|---|---|---|
| perf_run | PARTIAL | 标准 native 正式窗口、授权、资源记录与保护；缺 idle、严格零输出、多轴/长期及完整网络统计 |
| perf_capacity | SUFFICIENT（固定 source viewer 轴） | 调度、矛盾结果拒绝、H/F 重复、取消保存；不交付本轮 TEST 容量，不搜索 source 轴 |
| perf_validate | SUFFICIENT（声明的负对照） | invalid/expired/replayed/source_stop/cancel；不涵盖全部断连/拥塞/RTCP/codec/厂商故障 |
| perf_publish + existing | PARTIAL | 合法 source generations、少量真实 Go churn；publisher live/ready 本身不证明保持阶段连续两轨或所有旧消费者终态 |
| RTMP/RTSP/FLV/WHEP native | SUFFICIENT（包/消息进展） | 独立凭据、有限窗口、真实计数与清理；不是 decoder，不覆盖全部 WebRTC codec |
| HLS client | PARTIAL | 新分片实际下载；双轨解码、帧/采样连续性需要独立证明 |
| GB sender client | PARTIAL | 三 transport、真实 PS/PES 双轨与清理；不是 SIP/厂商/全部 codec/解码验收 |
| perf_profile cpu/offcpu | PARTIAL | 对既有 PID 的有效采样与身份记录；TEST 权限/工具与实际高负载样本未执行，缺失须 BLOCKED |
| perf_profile heap-report | PARTIAL | 已有真实 dump 的报告；不创建 allocator 阶段快照，不自动证明 dump 与 ELF 来源匹配 |
| package_runtime | SUFFICIENT（声明的 runtime 闭包） | SHA、ELF/脚本/fixture/非glibc库和 launcher；不含 Chrome/Playwright/perf/BCC/jeprof/完整模拟器环境 |
| 旧 lifecycle/browser/codec/GB wrappers | INSUFFICIENT（当前完整 E2E） | 旧授权契约仍 BLOCKED，help 成功不恢复正常入口；不复制 framework 或关闭认证使其假绿 |
| 长期/网络归因整体组合 | INSUFFICIENT | 必须先补充上述采样、持久资源、接口指标和可执行验证契约 |

## 优先级与正式准入

独立审查提出的 P0 工具缺口已经以最小修复关闭。源码与原始离线/真实结果分别核对如下；mock/函数片段回归不冒充真实容量或媒体测试。

| P0 | 原可达缺口 | 最终保护与 GREEN 边界 |
|---|---|---|
| 被测 PID 身份 | existing 媒体正常时可给另一个 PID 计 CPU/PSS；采样没有 starttick/exe/netns | existing 校验实际 `/proc/PID/exe` 与 `--server-bin` 为同一文件；采样前后稳定 identity，外部 publisher 要求 starttick；wrong PID 在 probe 前 BLOCKED，identity 变化拒绝继续 |
| 建立期压力停止 | 预算只在 measurement 生效，发布/建立/ready/warmup 没有保护 | pre-launch、准备等待、ready/warmup、测量均检查预算；过高安全 floor 在 fixture/launch 前 BLOCKED；真实最终 smoke 使用非零预算并保留36个预算采样 |
| UDP drop 假 F | socket 聚合从3降至0时 `-3` 被当作非零 server failure | 正差有失败证据，负差保留数值与解释、健康轮 INCONCLUSIVE/UNCONFIRMED，不伪装为0或SERVER_UNSTABLE |
| 固定 counts 继续施压 | RESOURCE_BUDGET/CLIENT_RESOURCE/UPSTREAM_FAILED/RUNNER_FAILED 后还跑更大 count | 固定档位遇非 HEALTHY/SERVER_UNSTABLE 边界立即停止，保存 INCONCLUSIVE/reason；五类边界回归只调用第一轮 |

DEV 证据根为 `/var/tmp/media_server_tooling_acceptance/`。`process-budget-udp-red.log` 保存旧缺口（含新增保护接口尚不存在的两个 ERROR）；`counts-safety-red.log` 保存安全边界后继续三档的 RED。`process-budget-udp-green.log` 与 `followup-build-cwd-green.log` 各28/28 GREEN；最终 `ctest-final-guard-green-parallel.log` 为59/59 PASS、99.52s。中间完整 CTest 58/59 失败源于新 subprocess 测试在非 Git cwd 未提供已有 BENCH_HEAD fixture；测试修复后通过，原失败保留，不归为生产问题。

`final-negative/result.json` 五类负对照5/5 PASS；`final-guard-smoke/result.json` RTMP→五输出各1viewer、8s PASS/HEALTHY，最小窗口覆盖为 HLS约0.999957。`real-signaling-final/result.json` 证明三个4s小 churn wave、下一代6s短窗、旧代预期退出、两代身份不同、receivers空与所有 own process 回收。RTSP/HLS独立ticket的 FFmpeg双轨decode退出0；124/95是 showinfo/ashowinfo匹配日志行数，不是精确帧数。Chrome仍为 **BLOCKED：本轮仅loopback的namespace中ICE gathering超时**；不称浏览器DTLS/视频/音频解码PASS。

P0 正式前置仍包括：准确 runtime/ELF/fixture 来源与 SHA、真实监听地址端口归属、独立本轮环境与授权、全部 viewer 真实 ready、公共实际窗口、非零预算、自有进程有界清理，以及 source/client/环境先失败时禁止 F 归因。operator 须保存同 ELF 多服务的 PID/监听归属，内置 exe/starttick/netns 稳定检查不能自动证明 URL 属于指定服务。

Pi手册给出的初始保护例为 free RAM≥8192MiB、free disk≥2048MiB、DUT PSS≤16384MiB；正式预检先确认适合本轮环境并固定，不能将预算变更隐藏在可比较轮次中，也不能把触预算的 BLOCKED当容量F。默认0仍只观测。`--server-bin`需指向实际运行的同一 ELF文件，另一份同hash拷贝也不满足samefile；必要时先保存真实exe路径。

P1：严格 idle/publish baseline；source 轴与二维增长；完整协议/codec 可执行功能计划；NIC/lo/veth Mbps/PPS、drop/retrans 差分；每窗及独立重复统计；global latency 样本与 HLS/GB 分位补齐；扩展 FFmpeg/Chrome 解码；长期采样保存、publisher 生命周期与 retention 后内存/FD/socket 观察。上述项完成前，仅允许按限定职责解读现有结果。

P2：profiling 输出的火焰图转换与版本记录、报告自动聚合、图表/索引易用性。先保持实际采样、单位、所有权与来源正确，不为了展示添加未经证明的性能优化。

实际执行顺序、命令、安全预算、停止操作见 [pi_handoff](pi_handoff.md)。每个新 TEST 场景需要独立准入记录；本轮交付工具和方案并不授权立即运行正式容量、超载或 soak。只有正式 TEST 的新原始结果才可改变该场景的 NOT_RUN 状态。
