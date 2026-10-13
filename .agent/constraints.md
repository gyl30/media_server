# 稳定工程约束

这里只记录已经接受的架构、生命周期和证据边界，依据五份文档中的决定、提交和源码可独立理解。格式、include、命名、提交消息等普通规范见已跟踪的 [development_conventions.md](../docs/development_conventions.md)；本地未跟踪的个人规则不是本文的必读依赖。

## C01：媒体核心保持既定模型

保持固定 tracks、source generation 和 live push。代际变化结束旧源，不恢复 reader/history/GOP replay、cursor、runtime track event 或反向关键帧控制。依据：[决策 D01–D02](decisions.md)。

## C02：状态必须有独立事实

新增 lifecycle 字段前说明它保护哪个可达时序、为何不能从已有资源或 identity 推导。一次性 startup 不额外防重；真实协议阶段、取消状态和 once-only 业务事实继续保留。依据：[决策 D05](decisions.md)中的提交与源码；本地 `docs/project_decisions.md` §6 仅补充历史说明。

## C03：抽象必须减少整体复杂度

只有存在共同职责、真实调用方且所有权更清楚时才统一实现；不以相似方法名或少几行代码为理由增加包装、callback/policy/config 层。废弃内部 API 直接移除，不保留兼容或 fallback 路径。依据：[决策 D07、D12](decisions.md)。

## C04：关闭服从 owner 与资源 lifetime

沿用 session 的 owner worker 清理、必要回调保活和终态断环。移除注册/订阅不撤销已经排队或 snapshot 的回调；取消也不保证成功完成的异步操作不会恢复。用现有资源/identity 守住实际边界，保留必要取消检查。依据：[决策 D06、D09](decisions.md)。

## C05：网络不隐藏恢复策略

transport 负责 I/O、串行队列、buffer lifetime 和错误报告；上层决定关闭或业务策略。常规协议/网络错误沿现有返回值或 error_code 传播，不另建异常控制流。保持现有 UDP datagram/endpoint/drop 与 TCP 背压契约，不通过重连、重试、延迟关闭、sticky error 或无限扩队列掩盖错误。依据：[决策 D04](decisions.md)。

## C06：授权与代际保护不能为测试让步

保持一次性 token 的原子认领、过期/操作/源 identity 校验和独立观看身份。先满足必要授权，再创建正式处理资源；不关闭鉴权、复用已消费 token 或恢复旧控制旁路。源替换后旧会话只能清理自己持有的代际。依据：[决策 D01、D12](decisions.md)。

## C07：结论对应真实证据范围

区分源码风险、模型实验、生产路径复现、测试环境限制和接受的设计。历史 PASS 绑定 HEAD、配置、负载和覆盖范围；媒体连接/字节增长不等于解码健康，测试上限不等于服务器最大容量。性能改动需可归因的同条件证据，不能从热点或 RSS 单独推出收益/泄漏。依据：已跟踪的 [系统验证](../docs/system_validation.md)与 [lessons.md](lessons.md)中可核对的提交和回归。

## C08：沿用已裁决的边界

端口池、WebRTC 支持范围以及已接受的异步控制/空闲语义、D13 限定的既有日志行为按 [决策 D08、D12–D13](decisions.md)处理。没有新需求或足以改变决定的新证据时，保持现状；历史问题台账不构成实施授权。当前明确需求优先于历史决定，冲突时说明影响。
