# 工具验收记录

## 范围与证据归属

BASE `b0b67da8a3f020f1ee8b15673d57f999042beb57`，origin/main `9486c8602ef3f5acdc63e4cafdb05ff6dec6c10e`。本轮独立分支/worktree：`codex/performance-tooling-acceptance`，`/var/tmp/media_server_tooling_acceptance/worktree`。原main和Pi/Claude工作区未改。

原始证据：DEV `/var/tmp/media_server_tooling_acceptance/`；TEST `/var/tmp/media_server_tooling_acceptance_20261012_codex/`。目录只作访问入口，原始数据不提交；认证URL和日志不得公开。HEAD/dirty与命令在各run、manifest和日志中分别保留，不把开发中dirty快照冒充最终clean包。

## 已完成测试

| 分类 | 实际执行 | 结果与边界 |
|---|---|---|
| Fresh build | GCC16、Boost1.92static、FFmpeg901SDK；RelWithDebInfo、BUILD_TESTING、MEDIA_SERVER_BUILD_BENCHMARKS | PASS；build/configure日志保留；误用BUILD_BENCHMARKS的初次配置已纠正 |
| CTest parallel | own `unshare -Urn`，`ctest -j8 --output-on-failure` | 59/59 PASS，99.79s；完整日志ctest-green-parallel.log |
| CTest serial | ownnamespace，`ctest --output-on-failure` | 59/59 PASS，282.87s；ctest-green-serial.log |
| Offline | followup、profile acceptance、publish acceptance | 21/21、9/9、1/1 PASS；后续最终方案审查保护的新增回归另记末尾，不能继承旧PASS |
| 真实媒体 | dev-tooling-integration.py调用现有perf_run，8s窗口、warmup1、progress2，独立namespace | 10正常场景全部PASS；最多双源FLV10/WHEP2，非容量数据 |
| 负对照 | perf_validate实际invalid/expired/replayed/cancel | 各PASS；初轮source_stop FAIL，不能把初次总体INCONCLUSIVE改写PASS |
| source_stop GREEN | 明确本轮publisher kill，保存真实执行时点，双独立FLV客户端 | PASS；source-stop-green目录及原始RED保留 |
| 真实Go授权/churn | perf_publish cycles2、perf_run existing少量viewer波次 | 三个4s波次各PASS；新generation短窗PASS；组合浏览器探针未完成，不能称完整E2E PASS |
| FFmpeg实际解码 | 独立新ticket RTSP/HLS、map视频/音频、showinfo/ashowinfo | 两项exit0；原始日志各有124个video showinfo行、95个audio ashowinfo行（行数不是精确帧数） |
| Chrome | Playwright原生RTC、ownnamespace仅loopback地址 | BLOCKED：ICE gathering timeout；不是DTLS/解码PASS，不以native替代Chrome |
| DEV runtime | package、tar、独立路径解包、逐SHA、helps、ownnamespaceRTMP→FLV1 | PASS：111 manifest项匹配，6s真实video179messages/audio281messages，全部ownedprocess回收 |
| TEST deployment | 仅新目录传输/解包/hash/ldd/help，无媒体服务或namespace | PASS：111项匹配；34项help中30PASS/4BLOCKED（Playwright依赖）；不等于TEST性能验收 |
| Profiling driver | mock有效/空计数、sample、folded、heap、PID、超时 | CODE VERIFIED；mock不是实际perf/eBPF/heap采样 |
| DEV actual availability | 自有短时Python进程，1s cpu-stat/offcpu | BLOCKED：perf_event_paranoid=4、BCC不可运行；未改系统守卫/配置 |
| TEST capacity/profiling/soak | 未执行 | NOT_RUN，交Pi；本轮无容量或性能基线结论 |

10正常场景：RTMP→RTMP/RTSP/FLV/HLS/WHEP；RTSP TCP→FLV；RTSP UDP→FLV；RTSP pull→FLV；WHIP→WHEP；GB UDP/TCP active/TCP passive各→FLV；RTMP→三个GB输出；双源FLV10/WHEP2。均为native合法媒体进展，非全部codec解码或64组合矩阵。

fixture为真实合成H264/AAC 720p30、GOP60（12s循环），WHIP H264/Opus；原fixtureSHA `3e100535299f428ee13dfde6ab789aaa1f326129932cbb4a4163a97c585761e8`，WHIP SHA `b40d0f074f040b52047277a7c357f9f39619a6eddf890852c916aef2fad3d4d0`。编解码详情在assets/fixture-ffprobe.json，不声称码率完全恒定。

## 保留的失败

1. 初次CTest误选系统/usr/bin/ffmpeg，HEVC FLV生成失败；显式MEDIA_SERVER_TEST_FFMPEG=/home/gyl/bin/ffmpeg后通过。原ctest-initial-parallel.log保留。
2. 严格profiling后原fake报告缺真实Samples/Total格式，followup失败；修正fixture契约，未放宽生产断言。ctest-final-parallel.log和followup-green.log中的旧失败保留。
3. source_stop原先graceful等待10s使实际断流晚于18s测量窗口；负对照正确拒PASS。改故障注入为owned publisher明确kill，正常stop_process未变。
4. 一次DEV help验收误输入不存在的gb_rtp_receiver.py；这是验收命令错误不是包缺文件。rtsp_publisher实现不支持--help，属于旧发生器已知边界。
5. 仓库外组合Go/Chrome探针的初次bind0.0.0.0配置不合法、affinity重叠、等待旧轮次造成错过新generation、Chrome loopback ICE超时；各目录real-signaling至real-signaling-05均保留。这些不能归类为生产故障，也不伪造整组PASS。

## 本轮已修工具问题

容量状态/边界/exit一致性；取消checkpoint；负对照全部组实际拒绝与已健康后实际停源；测量实际窗口覆盖；非交互stdin/-nostdin；publisher setup失败结果；空profiling拒假PASS与自有进程组有界清理；静态ELF ldd；launcher SHA、cache/submodule/dirty/CLI/build来源记录。没有生产修改、第三方变化、正式压力测试。

最终方案审查新增保护、最终clean运行包与最终Git快照在后续验收补记及`test_plan_review.md`中记录；原失败目录不删除、不覆盖。
