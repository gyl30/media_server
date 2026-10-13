# media_server 项目入口

Codex、Pi Agent 和 Claude Code 的首次任务提示应显式要求读取本文件，再按任务阅读以下文档；不假定工具会自动加载它们。

- 接手任务：阅读 [.agent/context.md](.agent/context.md) 和 [.agent/current.md](.agent/current.md)，了解项目及历史验证快照。
- 涉及架构、状态、生命周期或设计建议：阅读 [.agent/decisions.md](.agent/decisions.md) 和 [.agent/constraints.md](.agent/constraints.md)。
- 排查问题、选择回归或审查历史修复：阅读 [.agent/lessons.md](.agent/lessons.md)。
- 编码、整理格式或准备提交：阅读 [docs/development_conventions.md](docs/development_conventions.md)。
- 涉及媒体核心实现：阅读 [docs/media_core_architecture.md](docs/media_core_architecture.md)。
- 性能测试、容量分析或 profiling：阅读 [docs/performance_testing.md](docs/performance_testing.md)，再按工具读取 [docs/performance_tools/README.md](docs/performance_tools/README.md)。

`docs/` 保留当前工程规范、操作指南以及有独立价值的架构和验证文档；历史报告不自动作为当前待办，已删除的历史资料仍可通过 Git 提交追溯。未跟踪的性能原始报告仍由测试流程独立管理，不能因为整理文档而删除活动测试证据。
