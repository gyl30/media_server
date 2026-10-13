# 性能测试工具

本目录是当前工具的执行契约，不是容量成绩单。DEV 负责工具正确性、编译及短时功能验收；TEST 负责 Pi 的正式性能测量。环境与执行门槛见 [环境](environment.md) 和 [性能测试规范](../performance_testing.md)。

| 需求 | 入口与说明 |
|---|---|
| 固定输入、单/多源、单/混合协议及资源采样 | [perf_run.py](perf_run.md) |
| 递增、二分和独立复验 | [perf_capacity.py](perf_capacity.md) |
| 鉴权、源停止、取消负对照 | [perf_validate.py](perf_validate.md) |
| 真实信令发布及源替换 | [perf_publish.py](perf_publish.md) |
| 独立 On/Off-CPU 与 heap report | [perf_profile.py](perf_profile.md) |
| 运行包、依赖和 SHA | [package_runtime.py](package_runtime.md) |
| 所有当前协议客户端、微基准及辅助程序 | [协议客户端](protocol_clients.md) |
| 状态、证据等级、指标与可比性 | [结果格式](result_format.md) |
| 异常定位、取消与安全停止 | [troubleshooting](troubleshooting.md) |

正常播放工具需要当前授权。每个 viewer 使用独立一次性 token，不重用 URL；成功认领后任何失败也不回滚。自管 perf_run 使用协议真实的一次性 verifier fixture，existing/perf_publish 使用 Go signaling；二者证据等级不能混写。

native RTP/FLV 媒体检查不是浏览器解码。HLS 唯一分片下载不是逐轨解码。负对照永远不建立容量 F 边界。缺失采样、工具权限不足和客户端资源不足不能写成服务端容量失败。

旧 wrapper、合成发布器和旧生命周期脚本中的未授权 URL/旧参数不适用于当前授权流程；正常负载使用上述入口，不恢复兼容路径。历史工具验收可从 Git 追溯，新任务按实际结果判断，不从旧报告继承 PASS。

输出目录必须新建且不存在。结果与 URL 文件包含敏感 token；0700 是目录访问限制，不代表日志已脱敏。不要公开原始日志或提交运行证据。所有正式运行关联准确源码 HEAD、dirty 状态、ELF/fixture/manifest SHA 和实际测量窗口。
