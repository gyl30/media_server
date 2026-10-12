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
| 真实Go授权/churn | perf_publish cycles2、perf_run existing少量viewer波次 | 最终real-signaling-final完整非浏览器组合PASS：三个4s波次各FLV2/WHEP1、下一代FLV1/WHEP1的6s窗口、旧代退出、两代ID不同、receivers为空及所有ownedprocess回收；原先含Chrome的组合失败另保留 |
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
2. 严格profiling后原fake报告缺真实Samples/Total格式，followup失败；修正fixture契约，未放宽生产断言。followup-green.log保存该直接测试失败；对应旧完整CTest日志路径后被新回归使用，不能声称两份完整失败日志都保留。
3. source_stop原先graceful等待10s使实际断流晚于18s测量窗口；负对照正确拒PASS。改故障注入为owned publisher明确kill，正常stop_process未变。
4. 一次DEV help验收误输入不存在的gb_rtp_receiver.py；这是验收命令错误不是包缺文件。rtsp_publisher实现不支持--help，属于旧发生器已知边界。
5. 仓库外组合Go/Chrome探针的初次bind0.0.0.0配置不合法、affinity重叠、等待旧轮次造成错过新generation、Chrome loopback ICE超时；各目录real-signaling至real-signaling-05均保留。这些不能归类为生产故障，也不伪造整组PASS。

## 本轮已修工具问题

容量状态/边界/exit一致性；取消checkpoint；负对照全部组实际拒绝与已健康后实际停源；测量实际窗口覆盖；非交互stdin/-nostdin；publisher setup失败结果；空profiling拒假PASS与自有进程组有界清理；静态ELF ldd；launcher SHA、cache/submodule/dirty/CLI/build来源记录。没有生产修改、第三方变化、正式压力测试。

## 最终方案审查后的定向复核

- P0最小修复：existing DUT/外部publisher的starttick、ELF、netns身份核对；准备/建立/ready/warmup和测量统一预算；UDP socket drop聚合负差INCONCLUSIVE；固定counts遇到客户端、资源、来源或工具边界停止。对应process-budget-udp-red.log、counts-safety-red.log及GREEN均保存。
- 新增离线回归28/28，两次直接GREEN；从build的Python3.11运行曾因测试子进程缺Git cwd而两ERROR，完整CTest 58/59 FAIL保存在ctest-final-parallel.log。测试仅使用已有MEDIA_SERVER_BENCH_HEAD fixture，非Git cwd下28/28 GREEN（followup-build-cwd-green.log），未skip或修改产品逻辑。
- final-guard-smoke：同一合成输入、五协议各1viewer，8s完整窗口、warmup1；非零资源预算，PASS/HEALTHY，全部ownedprocess回收。这不是正式CPU/容量数据。
- final-negative：invalid、expired、replayed、source_stop、cancel **5/5 PASS**；各底层runner非零是预期失败，验证器以实际授权/中断/清理判据通过，不作服务端F。
- final-plan-only：手册示例start100/step250/granularity50/maximum5000静态计划NOT_RUN、exit0；原granularity100不符合整除要求，命令被明确拒绝，已修正文档，不运行正式搜索。
- real-signaling-final：status PASS、errors为空、旧源UPSTREAM_FAILED为预期、新源PASS，发布者两代均ready、devices_deleted、receivers_after为空、all_owned_reaped=true；RTSP/HLS独立ticket双轨解码均exit0，日志行数不能当精确帧数。Chrome仍单列BLOCKED。

最终全量CTest `ctest -j8 --output-on-failure` **59/59 PASS，99.52s**，在ctest-final-guard-green-parallel.log中另存；完整构建exit0，真实flags含 `-O2 -g -DNDEBUG -Werror`。该快照对应最终工具生产修复提交 `21bba8cf837eb2f19030b2dc4b09a94694cc80db`，后续仅文档。最终clean包证据将保存到final-runtime-evidence/，不覆盖原dirty运行包。manifest记录打包HEAD/dirty，ELF编译来源仍据真实build日志与SHA关联，不补造binary_source_head。
