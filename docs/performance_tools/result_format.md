# 性能工具结果格式和解释边界

以下描述当前源码，不强行统一不同年代工具的 JSON schema。归档必须同时保留原始结果和工具版本；不能只留汇总表。

## 状态

| 状态 | 含义 |
|---|---|
| PASS | 满足该工具自己的可验证判据；负对照PASS表示故障被正确拒绝，不是健康负载 |
| FAIL | 受测行为或工具验证失败；不自动等于服务器容量极限 |
| BLOCKED | 依赖、权限、客户端资源或操作员安全预算阻止有效继续 |
| UNSUPPORTED | 当前工具明确不支持的输入/codec，不是实现失败 |
| NOT_RUN | 尚未执行、plan-only或运行中checkpoint |
| INCONCLUSIVE | 证据不足、结果矛盾、客户端瓶颈、未复验边界或取消，不能造成功容量 |

成功退出码通常0，否则1/参数错误2。perf_capacity检验runner退出码、状态与HEALTHY/SERVER_UNSTABLE一致性。perf_validate的负对照runner非零可以是预期，但必须有具体故障证据和清理结果。

## perf_run result.json

| 字段 | 来源/语义 |
|---|---|
| `status`、`capacity_boundary` | 总体结果与边界类别，两者必须同时检查 |
| `head`、`config`、`auth_mode` | 工具仓库HEAD、实际参数、自管一次性fixture或真实signaling；不是实际server ELF版本的替代 |
| `fixture_sha256`、`fixture_metadata`、`whip_fixture_sha256` | 输入素材摘要与ffprobe记录 |
| `source_ids` | 本次实际流身份；可含敏感推流token |
| `preparation_seconds` | 建立与首媒体准备时间，不是稳态吞吐测量时长 |
| `requested_window` | 请求的共同 CLOCK_MONOTONIC 纳秒起止 |
| `groups` | 实际客户端组、requested、exit_code、measurement、first_media、log、first_errors、failure_category |
| `groups[].window` | 实际客户端起止、实际秒数、起止偏差、请求窗口重叠覆盖；固定required_coverage=0.99 |
| `samples`、`system_samples` | 带真实单调时刻的进程与主机资源快照，采样有耗时，不能假设恰好1Hz |
| `server_identity`、`process_identities`、`samples.*.identity` | 从 `/proc` 固定和复核的 starttick、exe、netns；身份改变拒绝采样替代进程，归类 BLOCKED/PROCESS_IDENTITY |
| `budget_samples` | 非零操作员预算下的 preparation/warmup/measurement 检查点，含实际单调时刻、memory_available_kib、disk_available_bytes、server_pss_kib；准备期节流至每秒，超限立即 RESOURCE_BUDGET |
| `process_metrics` | CPU/RSS/PSS/FD/线程区间聚合；保留方法、实际分母和thread_sample_window |
| `server_evidence` | 从测量窗口新增日志及采样提取queue-full、发送缓冲错误、UDP drops；缺日志不等于日志错误数已测为0 |
| `fault_evidence` | source_stop的`requested_monotonic_ns`/`completed_monotonic_ns`、ready组数、publisher PID与退出结果；实际完成必须发生在测量窗口内，无此证据不能说已执行有效故障 |
| `verification_calls` | 自管fixture收到的验证请求数，不等于独立viewer数或成功数 |
| `cleanup`、`processes`、`errors` | 本轮所有权清理结果、PID及退出码/脱敏命令、首个或多个异常记录 |

`groups[].measurement`保留协议原始字段，不能凭字段名称假设单位一致。Native TCP/FLV是有效音视频消息计数，WHEP是解密后的RTP双轨计数，GB是PS/PES进展，HLS是独立新分片进展。收到字节、握手完成、SDP201、轨道存在均不能独立替代媒体进展；这些native证据也不能代替Chrome/FFmpeg真实解码。

失败发生在建立期时没有完整measurement/window，必须保留缺失事实。被取消或缩短的窗口不能补成请求时长。容量驱动根result是搜索/复验的汇总，子result仍是原始证据；不得覆盖失败轮。

`server_evidence.udp_drops` 是当前存活 socket 累积计数的端点差，而非不会回退的全历史计数。socket 关闭可导致负差：保留负值及 `udp_drops_interpretation`，无独立服务器错误证据时记 INCONCLUSIVE/UNCONFIRMED；不能将负差算作服务器失稳，也不能截断为0并声称没有丢包。namespace 级网络计数不能自动归因给 DUT。

`--outputs none` 的结果仍包含整个测量窗口运行的每源 FLV 双轨客户端：这是 PARTIAL publish-only，不是零输出纯发布测量。其 CPU、网络、授权和清理成本均不能从结果中隐去。

## 单位与统计对象

- CPU cores = 区间CPU seconds / 实际wall seconds，1表示一核，乘100才是单核百分比；不是整机百分比。不同进程可能使用client_window_counter或proc_window_delta，保留方法标记。
- RSS/PSS/VmSize以KiB存储（1024字节）；RSS含共享页全额，PSS按共享比例，VmSize是地址空间，不能混称物理内存。FD是已打开描述符数，不等于socket数。
- thread CPU由对应采样端点差分，新出现线程缺少前端点时无法恢复完整区间成本，不能将0当作线程从未运行。
- steady_media_bytes通常不等于链路wire bytes；WHEP解密RTP计数和NIC/loopback吞吐不同。平均每viewer吞吐的分母是实际成功测量时间与viewer数，不能用全部请求数量稀释失败。
- UDP/TCP tx/rx queue为内核当前队列字节，不是应用层队列容量；UDP drops为采样时仍属于进程的socket聚合值，关闭socket可能使聚合减少。namespace SNMP属于整个namespace，不能全部归因server。
- p50/p95/p99是该字段声明的样本群（首媒体延迟、每viewer吞吐等），不是统一的请求延迟。首媒体延迟包含建立过程，不能和稳态发送耗时比较。
- allocator allocated/active/resident/retained、Linux RSS/PSS不同；retained不是活跃对象或泄漏证据。

## 归档与可比性

运行包 `manifest.json`、SHA256清单负责记录实际源码HEAD/dirty、二进制、脚本、共享库、fixture的版本。运行结果必须关联所用包SHA、命令、环境、affinity、worker、起止时间、原始日志和profile配置。归档前逐项校验SHA，不静默混入另一版本host工具。

同条件、同媒体、同窗口、同allocator、同机器/网络隔离、无profiler的结果才能直接比较。DEV功能测试不等于TEST性能验收；loopback不等于跨机NIC容量；CPU profiler/OffCPU/heap分别运行，不把诊断开销混入正式基线。

AUTHORIZATION、CLIENT_STARTUP、FIRST_MEDIA_TIMEOUT、MEDIA_STALL、SOURCE_ENDED、UPSTREAM_FAILED、SERVER_UNSTABLE、CLIENT_RESOURCE、RESOURCE_BUDGET、CLEANUP_FAILED等字段以原始日志支持；UNCONFIRMED不能升级为服务器瓶颈。monitor缺失/无权限是BLOCKED，不能记零成本。

原始JSON/log/credentials可能含token、URL、认证材料，0700运行目录只限制访问，不意味着内容已经脱敏。原始受限证据与可公开脱敏报告分别保留，不将未经审查证据提交Git。
