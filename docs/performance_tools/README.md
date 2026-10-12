# 性能测试工具

本目录是当前工具的执行契约，不是容量成绩单。DEV 负责工具正确性、编译及短时功能验收；TEST 负责 Pi 的正式性能测量。详见 [环境](environment.md)、[本轮验证](verification.md) 和 [Pi 交接手册](pi_handoff.md)。

| 需求 | 入口与说明 |
|---|---|
| 固定输入、单/多源、单/混合协议及资源采样 | [perf_run.py](perf_run.md) |
| 递增、二分和独立复验 | [perf_capacity.py](perf_capacity.md) |
| 鉴权、源停止、取消负对照 | [perf_validate.py](perf_validate.md) |
| 真实信令发布及源替换 | [perf_publish.py](perf_publish.md) |
| 独立 On/Off-CPU 与 heap report | [perf_profile.py](perf_profile.md) |
| 运行包、依赖和 SHA | [package_runtime.py](package_runtime.md) |
| 所有当前协议客户端、微基准及辅助程序 | [协议客户端](protocol_clients.md) |
| 功能验证入口及浏览器边界 | [功能验证](functional_verification.md) |
| 旧生命周期工具、可用替代流程 | [生命周期](lifecycle_verify.md) |
| 所有旧独立 wrapper 与替代命令 | [旧入口](legacy_tools.md) |
| 审计、结果解释、异常处理 | [audit](audit.md)、[result_format](result_format.md)、[troubleshooting](troubleshooting.md) |

正常播放工具需要当前授权。每个 viewer 使用独立一次性 token，不重用 URL；成功认领后任何失败也不回滚。自管 perf_run 使用协议真实的一次性 verifier fixture，existing/perf_publish 使用 Go signaling；二者证据等级不能混写。

native RTP/FLV 媒体检查不是浏览器解码。HLS 唯一分片下载不是逐轨解码。负对照永远不建立容量 F 边界。缺失采样、工具权限不足和客户端资源不足不能写成服务端容量失败。

输出目录必须新建且不存在。结果与 URL 文件包含敏感 token；0700 是目录访问限制，不代表日志已脱敏。不要公开原始日志或提交运行证据。所有正式运行关联准确源码 HEAD、dirty 状态、ELF/fixture/manifest SHA 和实际测量窗口。
