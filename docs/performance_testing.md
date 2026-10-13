# media_server 性能测试执行规范

本文保留正式性能测试的环境约束、测量方法和验收门槛。工具参数、部署命令和结果字段以当前源码及 [性能工具目录](performance_tools/README.md) 为准，不在这里维护第二套操作流程。

本次按 `a3d5490` 核对。本文不是测试结果；开始新任务时重新检查 HEAD、dirty、工具参数及实际环境，不将历史验证当作当前容量成绩。

## 1. 阅读与执行入口

| 任务 | 现行说明 |
| --- | --- |
| 协议、codec 和客户端媒体检查边界 | [协议客户端](performance_tools/protocol_clients.md) |
| DEV 构建、TEST 环境与归属 | [环境](performance_tools/environment.md) |
| 打包、传输、依赖与 SHA 校验 | [运行包](performance_tools/package_runtime.md) |
| 单/多源、单/混合协议与共同测量窗口 | [perf_run](performance_tools/perf_run.md) |
| 真实 Go 授权、发布及源替换 | [perf_publish](performance_tools/perf_publish.md) |
| 鉴权、源中断、取消负对照 | [perf_validate](performance_tools/perf_validate.md) |
| 递增、二分和 H/F 复验 | [perf_capacity](performance_tools/perf_capacity.md) |
| On-CPU、Off-CPU、heap report | [perf_profile](performance_tools/perf_profile.md) |
| 状态、单位、采样和归因 | [结果格式](performance_tools/result_format.md) |
| 异常定位、取消与安全停止 | [异常处理](performance_tools/troubleshooting.md) |

逐 viewer 凭据、共同窗口、GB 三种 transport 和运行包依赖已由现行工具实现，不再列为准备开发任务。正常负载使用上述入口，不使用旧 wrappers 的未授权 URL 或旧参数。各工具的 `--help` 可核对当前参数，帮助成功不证明媒体验收。

## 2. 双机职责与固定版本

| 机器 | 职责 |
| --- | --- |
| DEV：`gyl@172.20.45.187` | 读取主仓库 `/home/gyl/example/media_server`，创建自己的独立 worktree，构建、打包、部署和归档；正式性能不在 DEV 测量 |
| TEST：`root@172.20.63.32` | 校验运行包，在自己拥有的 namespace 中执行正式负载、容量和 profiling；不就地修改生产源码 |
| 摄像机：`172.20.63.201:554` | 仅在测量窗口外提供录像；凭据使用私有配置，不作为封闭 namespace 的实时上游 |

DEV 用既定 GCC 16、Boost 1.92 静态和 `/home/gyl/ffmpeg901` SDK 构建，FFmpeg CLI 与 SDK 是不同资产。具体 CMake 参数和包命令按环境、运行包文档执行，不使用他人的活动 `build/`。TEST 的依赖、内核、拓扑和可用资源每轮重新核验。SSH 或依赖失败记录 `BLOCKED`，不转到 DEV 冒充 TEST 结果。

从准确提交创建独立 DEV worktree。记录主仓库 HEAD/dirty，但未提交工作不自动进入基准；需要比较时显式引入实验 patch。不同版本使用独立构建和运行包，不覆盖基线 ELF、fixture 或原始结果。运行包含动态依赖，必须经 `run.sh` 设置其库环境并在 TEST 核验逐文件 SHA、loader 和实际启动，不能只复制服务端 ELF。

同一批可比运行固定源码、ELF、脚本、依赖、allocator、媒体 SHA、source/viewer 分布、CPU、worker、网络和采样条件。manifest 的打包源码 HEAD 不自动证明 ELF 的编译提交，应关联构建日志、CMakeCache 和实际 ELF SHA。

## 3. 固定素材与协议边界

正式基准复用已经校验的摄像机派生文件，不每轮重新录制或实时编码。首次准备在测量窗口之外：保留原录像，离线生成标准 H.264/AAC fixture，记录转换参数、FFmpeg 版本、ffprobe JSON、SHA256、码率、fps、GOP、音频参数及时间戳特征。原约定的标准目标为 720p30、约 2 Mbps、GOP 60、AAC 48 kHz 双声道；最终以固定文件的实际值为准。

现行自管 `perf_run` 检查 H.264/AAC fixture；WHIP 另需 H.264/Opus 离线文件。WHIP 入站 Opus 转 AAC，WHIP→WHEP 包含 Opus→AAC→Opus；RTMP AAC→WHEP 是共享 AAC→Opus，不能混为同成本负载。文件发布复用工具的真实时间发送与 copy 路径，不在正式窗口重新生成素材。合成 fixture 可用于工具功能验证，不能冒充真实摄像机正式基准。

当前八种输入、八种输出的工具入口见 [perf_run](performance_tools/perf_run.md)，生产范围见 [.agent/context.md](../.agent/context.md)，codec 与客户端覆盖边界见 [协议客户端](performance_tools/protocol_clients.md)。这不代表所有组合都已实测。`existing` 是使用外部源的工具模式，不是第九种输入协议。RTSP UDP publish 不代表 UDP play/pull；当前播放、拉流均为 TCP。GB active/passive 要分别覆盖建连方向；GB 网络 PS 验证不等于 SIP/厂商互操作。其他 codec/profile 单独验证，工具不支持与生产不支持分别记录。

先做所需组合的 1 source × 1 viewer 媒体 smoke，未验证组合不直接进入容量。优先以固定 RTMP 输入比较各输出边际成本，其他输入另组比较。拉流/GB 的上游和接收端都放在同一 namespace；实时摄像机直连实验与封闭基准分开。HLS viewer 按 playlist/唯一分片完整下载计量，不能当作一条持续推送连接。

## 4. 网络、CPU 与安全预算

正式测试使用 TEST 上本轮唯一的 namespace，仅配置 `lo` 和 `198.19.0.10/.11`，无 veth、NAT、default route，不访问摄像机或互联网。工具不自动创建 namespace；先核对名字未被占用，再创建：

```bash
# TEST root；RUN_ID 沿用本轮 DEV 记录。
NS="msperf-$RUN_ID"
ip netns add "$NS"
ip -n "$NS" link set lo up
ip -n "$NS" address add 198.19.0.10/32 dev lo
ip -n "$NS" address add 198.19.0.11/32 dev lo
ip netns exec "$NS" ip -br address
ip netns exec "$NS" ip route
ip netns exec "$NS" ss -lntup
```

从包根目录以 `ip netns exec "$NS" ./run.sh python3 bench/perf_run.py ...` 进入运行。显式设置 `--host 198.19.0.10 --client-address 198.19.0.11 --workers 8 --server-cpus 0-7 --client-cpus 8-15`；服务端和客户端亲和性由 runner 分别设置，不再使用旧 server wrapper，也不将整个 runner 绑到服务器核组。

上述 CPU ID 是 TEST 已核对的约定，每轮仍检查物理 core、SMT sibling、NUMA、可用 affinity 及其他负载。核对 DUT/publisher/client 各 PID/starttick/exe、每线程 affinity 和监听端口归属；同 ELF 不等于同服务实例，namespace 也不隔离 CPU 或内存带宽。仅 loopback 的成绩不能外推真实 NIC、RTT 或跨机 UDP 容量。

每次正式运行明确非零 `--min-free-memory-mib`、`--min-free-disk-mib`、`--max-server-pss-mib`，并记录客户端数、时长和磁盘上限；工具默认 0 只观测。预算要经当次预检选定，在可比组中保持一致。runner 已在准备、ready、warmup 和测量阶段检查预算，不把这项已完成保护列为开发待办。

监控整机可用内存、CPU/memory/I/O PSI、磁盘、FD、端口及客户端消费能力。触预算、潜在 OOM、持续异常队列/丢包、来源或客户端失效、影响其他服务时停止升档，保留首个失败。客户端先饱和、工具故障、身份不确定或环境不足不能构成服务端失败边界。系统参数、allocator 或内核调整需要单独授权并另立比较组。

## 5. 负载与执行门槛

| 模式 | 使用方式与边界 |
| --- | --- |
| 空载 | 独立持久 DUT，无 source/viewer；`perf_run` 没有 idle 模式 |
| 发布基线 | `--outputs none` 整窗仍有每源一个 FLV 双轨 viewer，只能称 publish+validation；严格零输出另用已有发布者和独立采样，不伪造纯输入成本 |
| 单源同协议 | `perf_run --sources 1 --outputs 协议:N` |
| 多源同协议 | 固定 source 数及实际 viewer/source 分配，不能只改总 viewer 数推断 source 成本 |
| 单/多源混合 | 显式输出比例；容量工具均分协议总量，不能外推任意混合分布 |
| churn / replacement | `perf_publish` 与 `existing` 短窗组合，每轮新 token；验证旧代结束与新代身份，不自动换流 |
| soak | 先核对 [perf_run 的长期运行边界](performance_tools/perf_run.md#长期运行边界)，另查 retention 与清理后资源；不能仅增大 duration 宣称长期工具已就绪 |

输入和输出使用独立有效授权，不能关闭鉴权或复用已消费 token。自管 runner 的一次性 verifier fixture 不等于真实 Go signaling；需要真实授权与源管理时使用 `perf_publish`/`existing`，并先做 `perf_validate` 负对照。native 消息/RTP/PS 进展不是解码，低负载另用现有 FFmpeg/浏览器验证必要路径。缺依赖或覆盖不足按工具边界记录，不能补写 PASS。

既定阶段与正式窗口保持如下。工具默认 `warmup=5,duration=30` 是 CLI 默认，不替代正式阶段参数：

1. **盘点**：冻结版本、CPU、fixture、namespace、依赖、预算和已有进程归属。
2. **Smoke**：重点路径 1×1，warmup 5–10 秒、测量 15 秒；检查双轨进展、授权负对照和清理。失败先诊断，不进入依赖它的高负载。
3. **正常基准**：空载、发布基线及 1/4/8/16/100 等可承受档位，每档显式 `--warmup 15 --duration 60`，至少 3 次独立运行，保存 median/min/max 和离散度。自管服务每次重启；持久 DUT 的 churn 是另一个场景。
4. **健康容量**：固定 source 数，使用 `perf_capacity` 的递增→二分→独立复验；先 `--plan-only` 检查计划。保留每次原始结果，详见下一节。
5. **过载诊断**：先分析首次失稳附近，再按 110%/125%/150% 的独立档位诊断，通常 15–30 秒。严重超载只有在安全、可诊断时才考虑，不用于常规吞吐排名。
6. **长时验证**：重点健康负载 30–60 分钟，观察资源、源更替和回收；不将短时容量 PASS 当长期稳定性 PASS。更长任务须先验证长期采样和源持续性。

## 6. 容量判定

保持原正式方案：从已验证健康档 `H` 每次增加 **500** viewer，首个可信服务端失败档为 `F`；然后按 **100** viewer 粒度二分，直到 `F-H <= 100`。H/F 均按粒度对齐；工具参数为 `--step 500 --granularity 100`。工具文档中其他档位只说明 CLI 用法，不自动改变本方案。

最后对 H 至少 3 次、F 至少 2 次独立正式复验，各用 `--warmup 30 --duration 90`。混合协议固定实际比例；容量驱动只搜索 viewer 轴，不能把它解释为 source×viewer 二维容量。复验矛盾或不单调记 `INCONCLUSIVE`；达到 maximum 仍无 F 只有健康下界。

同时检查 runner 的退出码、`status`、`capacity_boundary` 和原始错误。负对照 PASS、客户端/来源/预算/工具失败、取消及未确认失败均不建立 F。`overload_diagnostic_counts` 是候选计划，不自动加压；独立诊断按原约定取 `max(F, ceil(1.1×H/100)×100)`、`ceil(1.25×H/100)×100`、`ceil(1.5×H/100)×100`，先核验是否安全。

结论写成“当前版本、资源和媒体条件下，100 路粒度的最高已验证健康档 H / 最低确认失败档 F”，不声称数学最大连接数，也不泛化到其他编码、网络或浏览器。

## 7. 测量与健康证据

全部目标 viewer 实际 media ready 后，runner 建立共同 CLOCK_MONOTONIC barrier，再进入 warmup 和 measurement；授权/建立成本单列。检查 requested/actual window 及覆盖率，不能只凭请求 duration 计算指标。现行覆盖门槛为 99%，它是调度误差边界，不允许丢掉 1% 失败 viewer。

正式健康要求全部 requested viewer 建立、ready、持续 progressing，预期音视频轨持续增长，无非预期断连、上游故障、时间戳倒退、RTP 异常或解密失败；HLS 持续下载不同实时分片。检查每组原始结果和首次错误，不用连接成功或总字节增长替代。工具未观测的指标明确未覆盖，不能因为字段空缺写零错误。

每档保留以下证据，单位和来源以结果格式为准：

- server、publisher、client CPU 和每线程 CPU、上下文切换；CPU seconds / 实际 wall seconds 以核数报告。
- 初始/稳态/采样峰值/结束/清理后的 RSS、PSS、FD、线程和 socket；1 Hz 采样最大值不是真实峰值，VmSize 不是物理内存。
- 媒体 payload 吞吐、packet/message 进展、首媒体延迟与失败分类。不能把 payload rate 写成 wire Mbps，也不能平均组 p99 得到全体 p99。
- socket 队列/drop、namespace SNMP/TCP retrans 和必要接口计数的同窗差分；namespace 统计不自动独占归因 DUT。存活 UDP socket 聚合负差不可恢复历史，保留负值并记不确定，不伪造零丢包。
- 同窗系统 PSI、可用内存、磁盘以及客户端资源压力，保留采样开销与缺失端点。

现行 runner 已保存 server/publisher/client/system 采样、身份和原始窗口，不另建第二套采样流程。严格 idle/publish-only 和长期运行边界见 [perf_run](performance_tools/perf_run.md)，完整网络统计与结果解释见 [结果格式](performance_tools/result_format.md)，codec/真实解码边界见 [协议客户端](performance_tools/protocol_clients.md)；需要这些结论时先补足证据，不能沿用短窗 PASS。

## 8. 独立诊断与实验

正式基准不叠加重型 profiler。在相同 H/F 附近负载分别做 On-CPU、Off-CPU 和 jemalloc 诊断，使用 `perf_profile` 的现行模式、PID/starttick 与独立输出目录。诊断结果关联实际 ELF、fixture 和负载；缺权限、工具或有效样本记 `BLOCKED`，不当作零开销。

On-CPU 核对真实 counter/sample、用户/内核栈、符号及丢样；Off-CPU 核对进程过滤、等待单位与调度，正常 `epoll_wait` 不是瓶颈。heap-report 只分析已有 dump，不自动启用 profiling 或生成快照；先确认实际 jemalloc、profiling 能力和 dump/ELF 来源。仅给自有诊断 DUT 设置 allocator 配置，不污染 runner/publisher，不替换他人的 allocator。区分 live allocation、active/resident/retained 与 RSS/PSS，RSS 不回落本身不证明泄漏。

允许在自己的 DEV 实验 worktree 按既有授权验证已由证据支持的性能假设；不能因为看见热点就开展无依据重构。冻结 A，单一假设最小改 B，诊断插桩与正式 A/B 分开。保持同机器、CPU、worker、fixture、网络、负载、窗口和采样，串行独立运行，关键档位至少 3 次，推荐交错 A/B。

保存 diff、新文件、构建参数、ELF/脚本 SHA、失败和无收益结果；`git diff` 不包含未跟踪新文件，需单独核对并归档。若容量边界变化，按原规则重新验证 H/F；不得靠减码率、减 viewer 或放宽健康标准换取改善。实验不直接成为生产提交，不自动 commit/push/merge/cherry-pick。

## 9. 归档与清理

每轮使用唯一 RUN_ID；DEV worktree、TEST 包和结果目录均为本轮独立路径。最终证据回传 DEV 主仓库的 `docs/performance_reports/<run_id>/`，而非只留在将被清理的 worktree。`.gitignore` 已包含该目录，无需再次修改忽略规则；归档前检查 `git check-ignore -v docs/performance_reports/probe.json` 和 Git 状态。

报告按 [结果格式](performance_tools/result_format.md) 保存：实际命令、机器/namespace/CPU、HEAD/dirty、构建及包/manifest/ELF/fixture SHA、source/viewer 分布、真实窗口、逐组健康、资源 samples、全部失败和清理结果。保留正常 H/F 对照、On/Off-CPU/heap 原始证据或 BLOCKED 原因、瓶颈归因置信度、实验 patch 与未完成项。不要只交最大在线数，也不覆盖历史结果。

原始日志、URL 文件和 manifest 可能含凭据或路径；0700 不等于已脱敏。受限原始证据与可公开摘要分开，报告和性能原始数据不进 Git。

runner 只清理自己启动的进程与 receiver/sender；existing 的 DUT、signaling、publisher 仍由其 owner 管理。结束后核验 cleanup 字段、PID/starttime、端口和 namespace 残留。删除 namespace 名称不等于进程已释放，先处理本轮进程，再删除本轮 namespace；不使用全局 pkill，不干扰其他 Agent。

TEST 数据先回传并校验 SHA/完整性，再删除本轮临时包；DEV 的实验修改及新文件先归档，再按任务授权清理自己的 worktree/构建。发现未知归属或未保存修改时停止清理并报告，不以 force、reset 或全局 clean 处理。固定素材、最终报告和其他工作区保留。
