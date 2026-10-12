# 异常定位与安全停止

先保留失败目录、完整命令、result/log/manifest SHA，不用重跑覆盖。根据实际证据区分工具、客户端、上游和服务端问题。

| 现象 | 检查与处理 |
|---|---|
| HTTP 401/403 | 当前独立 token 是否签发/过期/已认领；控制 Bearer 与播放 token 不同；不要关闭认证 |
| HTTP 409 | 源尚未发布、已结束或 generation 不符；保留来源，不能偷换同名新流 |
| FIRST_MEDIA_TIMEOUT | 对应轨道是否真实到达；fixture/codec/关键帧；连接成功不是首媒体 |
| MEDIA_STALL/SOURCE_ENDED | 有无先健康媒体、实际停源时点、网络队列、对应客户端日志 |
| CLIENT_RESOURCE | client CPU/线程、FD、UDP drops；不是服务端容量 F 边界 |
| SERVER_UNSTABLE | DUT 退出、日志/队列/资源证据；负对照出现它应 INCONCLUSIVE |
| RESOURCE_BUDGET | 操作员安全预算触发；不是软件最大容量 |
| RUNNER_FAILED | 缺失/损坏 result、状态与退出码矛盾、超时；保留所有先前轮次 |
| CLEANUP_FAILED | owned process/receiver/sender 清单及退出码，PID/starttick 核验；不执行全局 kill |
| profiling BLOCKED | 工具是否存在、权限、PID 是否同一个、是否真正有 counter/sample；空文件不是零开销 |
| 旧 wrapper 出现403/旧参数 | 用正式 perf_run/perf_publish；见 legacy_tools，不恢复无认证兼容入口 |
| HLS 无媒体 | 检查307、独立token、playlist sequence推进及完整段下载；不能重复同一段凑进展 |
| TTY 改变/等待输入 | FFmpeg 必须 `-nostdin`，子进程 DEVNULL；报告工具缺陷，不掩盖 |

## 取消

对本轮保存的 runner PID 发送 `kill -TERM "$RUNNER_PID"`，等待其有限清理并核对结果。capacity 取消应保存 checkpoint/不确定汇总；validate 取消只回收自己创建的进程组。existing 模式不停止外部服务器/发布源，必须另行关闭自己拥有的 perf_publish 和服务。

如果需要强制停止，先核验 `/proc/PID/stat` starttime、cmdline 和所属本轮目录；仅终止确认归属的 PID/进程组。保留未完成状态，不把部分输出写成 PASS。禁止 killall、全局 pkill、删 Pi namespace/worktree、全局 /tmp 清理。

## 归档

原始日志、URL 文件、JSON、SDP 可能含凭据/密钥。结果在仓库外受限目录保存；公开前脱敏，但原始失败证据不覆盖。逐文件 SHA 与 manifest 的 SHA 一并保留。DEV 功能、TEST capacity、负对照和 profiler 各自独立目录，不混合性能统计。
