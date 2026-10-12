# 性能工具链审计

## 范围与证据

审计基线：`b0b67da8a3f020f1ee8b15673d57f999042beb57`；远端基线：`9486c8602ef3f5acdc63e4cafdb05ff6dec6c10e`。本轮工作区：`/var/tmp/media_server_tooling_acceptance/worktree`，分支 `codex/performance-tooling-acceptance`。不修改媒体、信令或第三方生产实现，不执行正式容量、超载或 soak，不占用 Pi 的 namespace。DEV 短时功能结果不等于 TEST 容量验收。

已检查三个基线提交、CMake 注册、bench Python/C++ 入口和现有工具测试。现有 followup 离线测试 13/13 通过，但下列新增 RED 表明它们尚不足以证明结果可信。23 个历史功能/协议 Python 入口的 `--help` 均退出 0；这只证明帮助可运行。

## 真实入口清单

| 类型 | 入口 | 审计结论 |
|---|---|---|
| 正式统一驱动 | perf_run.py | 自管一次性验证 fixture 或 existing 真实信令；9 种输入、8 种输出；有媒体 ready/barrier/资源采样；测量窗口需收口 |
| 正式容量驱动 | perf_capacity.py | 递增、二分、H/F 复验及 plan-only；仅离线 mock 验证，不执行真实容量搜索 |
| 正式负对照 | perf_validate.py | invalid/expired/replayed token、source_stop、cancel；需要修复误判 |
| 正式发布 | perf_publish.py | 真实信令、RTMP/RTSP TCP/UDP/WHIP、共享 fixture、多源、cycles；可用于源替换 |
| 正式诊断 | perf_profile.py | cpu-stat、cpu-record、offcpu、heap-report；必须验证非空有效证据 |
| 运行包 | package_runtime.py | 二进制、Python、fixture、非 glibc 动态库闭包；不自动包含 Chrome/Playwright/完整 Go 工具 |
| 正式播放客户端 | rtmp_fanout、rtsp_fanout、http_flv_fanout | 每 viewer 唯一凭据；默认双轨真实 media 回调/RTP 进展；RTSP 仅 TCP interleaved；native 不解码 |
| 正式 WebRTC 客户端 | whep_fanout、whip_publisher | ICE/DTLS/SRTP；WHEP 默认双轨进展；WHIP H264/Opus fixture；不是 Chrome 解码验收 |
| 正式 Python 客户端 | hls_client.py、gb_sender_fanout.py | HLS 唯一分片完整下载而非双轨解码；GB 控制鉴权、UDP/TCP active/passive、真实 PS/PES 双轨 |
| 微基准 | gb_ps_fanout.cpp/.py | 内部 PS/RTP callback，非网络互通，合成 payload 不证明可解码 |
| 旧 native 发生器 | rtsp_publisher | 旧 stream-prefix、合成填充视频、没有当前逐源凭据；不推荐正常媒体负载 |
| 故障辅助 | webrtc_disconnect | 定向建立/断开，不是性能入口 |
| 已被统一驱动替代 | rtmp_fanout.py、rtsp_fanout.py、http_flv_fanout.py、hls_fanout.py、whep_fanout.py、rtmp_publish.py、rtsp_publish.py、whip_publish.py、publish_only.py、mixed_fanout.py、gb_network.py、performance_baseline.py | 旧无 token URL 或删除的参数，当前不可作为可执行性能入口；用 perf_run/perf_publish，不增加兼容 wrapper |
| 功能回归（尚有授权迁移缺口） | lifecycle_verify.py、codec_interop_verify.py、browser_webrtc_verify.py、udp_rtcp_verify.py | 后三者依赖旧 Run；有解码/getStats 价值，但不能以帮助成功冒充媒体通过 |
| GB/UI 功能回归（尚有授权迁移缺口） | gb_signaling_verify.py、gb_simulator_verify.py、gb_web_ui_verify.py | 旧启动鉴权参数、play_id 和无 token FLV 预检，不能直接作为当前验收入口 |
| UI 展示验证 | gb_web_ui_visual_verify.py | response fixtures/截图/accessibility，不证明媒体行为 |
| 公共库 | fanout_support.py、perf_support.py、clients/*、credential_file/media_fanout/media_progress/packet_health/measurement_start headers | 非独立 CLI；保留现有基础设施 |

## 必须修复的实证缺口

| 工具 | 现有行为与可达证据 | 影响 | 最小修复及验证 |
|---|---|---|---|
| perf_capacity | mock FAIL/HEALTHY/exit1、PASS/SERVER_UNSTABLE/exit0、BOGUS/HEALTHY 均被接受 | 错定 H/F 边界 | 校验状态/退出码/边界；矛盾结果 INCONCLUSIVE；增加离线 RED/GREEN |
| perf_capacity | summary 仅最后写，取消丢汇总 | 已有轮次不可便利恢复 | 每轮 checkpoint；取消保留不确定汇总，只收自有进程组；mock 验证 |
| perf_validate | 两组中一组拒绝、另一组正常媒体仍 PASS | 授权负对照假阳性 | 全部组实际拒绝、未 ready；离线混合组回归 |
| perf_validate/perf_run | source_stop 仅配置分类，无实际执行/稳态证据亦 PASS | 启动失败伪装成预期中断 | 保存实际故障时点，要求先健康后中断；离线及短时真实负对照 |
| perf_run | 客户端有 start/end 日志，但只核对 ready/progressing | 短测量可当完整窗口 | 使用已有单调时间证据核对时长/公共窗口，记录偏差；不足不 PASS |
| perf_profile | /bin/true 做 cpu-stat 时 0 字节输出仍 PASS（已执行 RED） | 无采样伪成功 | 验证实际 counter/sample/folded 证据；缺失 BLOCKED；fake profiler 回归 |
| perf_profile/perf_validate | 子进程继承 stdin | 交互/TTY 风险 | DEVNULL；确保有限超时和自有进程组清理 |
| package_runtime | run.sh 未在 manifest.files | 启动入口不可校验 | 纳入 SHA 清单，解包逐文件检查 |
| package_runtime/perf_run | 仅 source HEAD/fixture SHA，缺 binary/build/dirty 来源区分 | 构建产物可能错标 | 记录实际 hash/cache/工具信息，明确源 HEAD 不等于未经证明的 ELF 编译 HEAD |

## 可用能力与保留边界

- native 公共客户端已有有限超时、全部 viewer media ready 后 warmup、单调 barrier、每轨进展、RTP 序号/时间戳检查，不重写。
- UDP/TCP 容量、媒体端口和 CTest RESOURCE_LOCK 语义保持。DEV 用本轮用户 network namespace 隔离，不能修改其他 Agent 环境。
- HLS 持续下载不同实时分片是有效的轻量客户端契约；双轨是否真正可解码另用小规模 FFmpeg 样本证明，不能宣称 HLS client 本身逐轨解码。
- perf_publish 的 cycles 已有同逻辑设备重发、独立 generation；源替换/churn 复用它，不创建生命周期框架。
- 输出 none 仍会运行一次源 FLV 双轨探针；必须写清，不称严格零输出。
- 资源预算默认 0 只记录；Pi 正式测试必须明确设置预算。UDP drop 为存活 socket 采样差值，不是所有历史 socket 的完整累计。
- 旧回归授权迁移是一项实际缺口；本轮优先保证正式入口。未修入口明确 NOT_COMPATIBLE/NOT_RUN，不向 Pi 提供假可执行正常示例。
- profiler 无权限/工具缺失为 BLOCKED，不修改全局配置来制造 PASS；正式高负载 profiling 留给 Pi。

后续验证及修复结论见各工具文档、`verification.md` 和 `pi_handoff.md`。审计中的 RED 原始证据保留仓库外本轮目录，不提交原始凭据、媒体或性能数据。
