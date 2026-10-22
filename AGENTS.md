# media_server 项目入口

首次接手先读取本文件，检查 Git 基线和工作区，再阅读 [.agent/context.md](.agent/context.md) 与 [.agent/current.md](.agent/current.md)。其中的提交、测试和报告属于历史快照；当前实现以源码及下列文档为准。

- 架构与模块边界：读 [architecture.md](docs/architecture.md)、[decisions.md](.agent/decisions.md) 和 [constraints.md](.agent/constraints.md)。
- 状态、并发、取消和资源归属：读 [media-lifecycle.md](docs/media-lifecycle.md)。`stream_id` 全生命周期唯一，结束后不复用；与 session 身份、播放器本地 generation 分别处理。
- 协议接入与媒体输出：读 [protocols.md](docs/protocols.md)。
- 信令、Web 或桌面调用：读 [signaling-api.md](docs/signaling-api.md)。
- 编码、构建、部署或提交：读 [build-deploy.md](docs/build-deploy.md)，遵守 `.clang-format` 和本次任务授权。
- 性能测量、容量或 profiling：读 [performance.md](docs/performance.md)。
- 排查问题或核对历史修复：读 [lessons.md](.agent/lessons.md)，按所列提交查证已删除的历史测试与报告。

第一方旧测试和 CTest 注册已清理。按任务执行实际需要的构建与验证，报告真实范围，不把历史 PASS 当作当前结果，也不自动恢复旧测试。

`docs/` 维护上述六篇当前文档；`.agent/` 保留设计决定和历史经验，不自动转为待办。`designs/`、已有 build、`docs/performance_reports/`、`docs/verification_results/` 及其他未跟踪或忽略的原始数据由本地使用者管理；清理已跟踪文档时保留这些内容。`third/` 按任务授权处理，不顺带修改依赖。
