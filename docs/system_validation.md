# 系统验证与验收记录

验证日期：2026-10-08。本文记录本轮实际结果与边界；未执行项目不视为通过。

## 基线与证据边界

- BASE_HEAD：`72b7bc2fe12b5c8a982aa7634d81d15ac6b97e6b`。
- 当前生产 CODE_HEAD：`cc63c0dbad482fe0e71164b6833229dd3c6c5615`。
- 本轮生产提交：`修复 WebRTC 候选地址切换后的媒体传输`。
- 生产修改仅 WHIP/WHEP 两个 session 文件，删除 11 行，新增 0 行。
- 测试工具提交 `998ddbf1d105346992fc669a161f2aa455277928`：修复运行包性能验证工具，7 个 bench 文件新增 29 行、删除 9 行。
- 未新增生产状态、抽象、配置、后台任务或自动重试；未修改 third、signaling 或媒体 fanout。
- 所有提交只保留本地，未 push；最终 HEAD 与 Git 状态以交付报告为准。

原始命令、日志、JSON、SHA256 与环境信息位于开发机和测试机的本轮独立目录：

```text
/tmp/media_server_validation_72b7bc2_eaybL57C/
```

原始数据不加入 Git。此前清理的历史报告不作为本轮运行证据。
早期 browser/UBSan GREEN 在修复尚未提交的工作区执行，JSON `head` 仍为 `72b7bc2`；
其修复状态与命令保留在原始证据中，原 JSON 未改写。UBSan 使用独立插桩构建，
不能与普通 Release 二进制 SHA 混用。
后续跨机验证使用 `cc63c0d` runtime-green 包，单独记录其 manifest 和 binary SHA。

## 五阶段状态

| Stage | 当前状态 | 实际证据 | 尚未完成 |
| --- | --- | --- | --- |
| 1 基线与 Sanitizer | PASS；ASan BLOCKED | CTest、Go、真实浏览器、UBSan | ASan 兼容 Boost.Context 构建 |
| 2 真实互通与跨机器 | 跨机代表路径 PASS；厂商 PARTIAL | simulator、大华 SIP/RTSP/生命周期收口 | 大华 Chrome 协商失败；原 SIP 认证未恢复 |
| 3 长期稳定性 | PARTIAL；原 GB observer FAIL 保留 | 混合 soak、GB native 与资源收口 PASS | 正常 DTLS close 的日志误分类 |
| 4 性能 | 汇总 COMPLETE；原 supervisor FAIL 保留 | 原 non-WHEP 39 次、新隔离 WHEP 9 次 | 无历史同条件 A/B，不宣称性能无退化 |
| 5 部署与验收 | 代表性验收 PASS | fresh 构建、preflight、测试机本地 recovery/WHIP/WHEP | 最终证据传输核对与本轮构建清理 |

本表不将 BLOCKED、NOT_RUN 或历史测试写成 PASS。

## 环境与构建

开发机 `172.20.45.187`：Ubuntu 22.04，Linux 6.8.0-136，i7-13700KF，31 GiB RAM。
测试机 `172.20.63.32`：Ubuntu 22.04.5，Linux 6.8.0-84，Xeon Silver 4216，32 logical CPUs，93 GiB RAM。
两机环境以本轮保存的检查输出为准；没有改变 sysctl、防火墙或系统动态库。
测试机最新 11:56 UTC 快照显示 Swap 2.0 GiB、used 0；不沿用早期摘要中的无 swap 描述，没有修改系统配置。

编译器 GCC 16.0.1 experimental、CMake 3.31.10；Boost 1.92 静态。
FFmpeg 测试程序为 7.1.1；编译所用 FFmpeg 库来自 `/home/gyl/ffmpeg901`：
libavcodec 63.1.101、libavutil 61.1.101、libswresample 7.1.101、libswscale 10.1.101。
不能将测试程序版本直接当作编译库版本。

libSRTP 由 `third/libsrtp` 子模块源码静态构建，固定提交 `ee1a77c9f9dc02c42bda9901038c500c5efe4cfa`。
项目 OpenSSL targets 选用 `libssl.a`/`libcrypto.a`，但 FFmpeg 依赖闭包仍引入 `libcrypto.so.3`。
spdlog、fmt、jemalloc、C++ runtime 等仍存在动态依赖；本轮不是全静态二进制。

Stage 1 Release 使用 `-O3 -g -DNDEBUG`。最终独立构建 `final-review` 为 RelWithDebInfo：

```bash
PKG_CONFIG_PATH=/home/gyl/ffmpeg901/lib/pkgconfig \
cmake -S . -B /tmp/media_server_validation_72b7bc2_eaybL57C/final-review \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBoost_DIR=/usr/local/lib/cmake/Boost-1.92.0 -DBoost_USE_STATIC_LIBS=ON \
  -DOPENSSL_USE_STATIC_LIBS=TRUE -DMEDIA_SERVER_BUILD_BENCHMARKS=ON \
  -DMEDIA_SERVER_TEST_FFMPEG=/home/gyl/bin/ffmpeg -DBUILD_TESTING=ON
cmake --build /tmp/media_server_validation_72b7bc2_eaybL57C/final-review -j12
```

实际媒体代码 flags 含 `-O2 -g -DNDEBUG -Werror`；二进制有 debug info，未 stripped。
第三方 C 代码使用其既有 warning 设置，不宣称所有依赖都启用了同一组 `-Werror`。
final-review media_server SHA256：`76344948901592afa7572f2c9d7f4db1815cd9962e2dac08c20cf94470b3b10f`。
上述 build 路径仅记录实际执行命令，不承诺清理后仍可用；最小配置/flags/link/CTest 证据与 7 个 UBSan 代表 ELF 已单独归档。

## Stage 1：基线、缺陷修复与 Sanitizer

普通基线 CTest 19/19；修复后的 Release CTest 19/19。
fresh final-review CTest 19/19，22.74 秒。
Go 普通测试、vet、race 严格串行：两包 test PASS、vet 退出码 0、两包 race PASS。
没有将 Go race 描述为 C++ sanitizer。

确认的生产 RED 是认证后的 ICE candidate tuple 变化被旧 guard 拒绝：
ICE/DTLS/SRTP 已建立，但 Chrome 改用新 tuple 后，媒体持续被丢弃，下游无帧。
最小修复删除旧地址拒绝 guard；仍校验当前会话 USERNAME、MESSAGE-INTEGRITY、FINGERPRINT 和 role。
只有认证后的 `USE-CANDIDATE` 更新 endpoint；普通 check 不更新，非当前 endpoint 媒体仍拒绝。
没有新增 ICE restart、自动恢复或上游关键帧请求。

旧二进制 STUN probe 在第二次合法 nomination 超时；修复后二次 nomination 正常。
坏 MI、错误 role、未知必需属性仍被拒绝。
真实 Chrome WHIP GREEN 经 RTSP 解码 600 帧，音视频时间戳持续且无回退；WHEP 真实解码增长。

fresh UBSan 构建及 CTest 19/19 PASS，浏览器 WHIP/WHEP 和代表性 lifecycle smoke PASS。
覆盖 RTMP、RTSP、HTTP-FLV、HLS、WebRTC 与 GB UDP/TCP active/passive。
受检日志 `UBSAN_REPORTS=0`；不能据此证明不存在所有内存问题。
Boost、FFmpeg、libSRTP 等未插桩第三方静态依赖不属于完整 sanitizer 覆盖。

ASan BLOCKED：现有 Boost.Context 不满足项目要求的 ucontext 与 sanitizer 构建条件。
没有关闭 compatibility guard、修改 Boost.Context 或增加 workaround。

## Stage 2：真实协议与跨机证据

本机 GB signaling 14 checks、simulator correctness 10 checks、recovery 6 checks、Web UI 37 checks PASS。
这些是 `LOCAL_LOOPBACK` 自有 simulator 证据，不是厂商互通。

`stage2-green2/result.json` 保存本轮 `CROSS_HOST` 验证：

- 开发机 RTMP publisher → 测试机 → 开发机 RTSP/HLS 解码。
- 开发机 RTSP publisher → 测试机 → 开发机 HTTP-FLV 解码。
- 测试机 RTSP pull 从开发机拉流，再由开发机消费测试机输出；删除后 source/session 404。
- 开发机 GB UDP sender → 测试机 receiver → 开发机 Chrome WHEP 音视频增长。
- GB TCP active/passive 跨机 relay → 开发机 HTTP-FLV 解码。
- 开发机真实 Chrome WHEP：remote candidate 为测试机地址，真实音视频解码增长。
- 开发机 Chrome WHIP → 测试机 → 开发机 Chrome WHEP：显式 encoder pause/resume 变体通过。

原始 WHIP 跨机尝试没有下一自然关键帧，后加入 viewer 无视频输出，原失败保留。
显式 resume 是测试 publisher 的操作，不是服务器 PLI/FIR，也不改变自然关键帧策略。
该链路结论为 `PASS_WITH_EXPLICIT_RESUME`，不将原始尝试改写为 PASS。
Chrome 网络 flag 未成功隔离多 NIC；实际网络选择以 candidate pair 记录为准。
两次临时 probe 的 readiness/模块名遮蔽错误保留，修测试工具后重跑，没有为此改生产代码。

新增真实大华设备验收为 PARTIAL，不能将设备整体写成 PASS。原 SIP 平台为 `172.20.63.30`，原密码掩码不可恢复。
最新只读 GUI 刷新证据为 `camera-dahua/camera-ui-readonly-final.json`；三次 RPC2 响应没有可恢复原密码。
用户随后明确授权直接设置，原 SIP 密码不能恢复也接受；不再将等待原 secret 作为继续测试的阻塞。
首次 GUI IP widget 的 Ctrl+A 自动跳格导致平台地址误写为 `172.172.63.32`，未收到 REGISTER，原失败保留。
首轮平台退出码为 0；SSH/controller 返回 255 的 buffered-stdin 问题单独记录，不据此修改生产代码。
第二次使用 native input selection 纠正地址后，实际测试平台 `172.20.63.32` 已保存并刷新确认。
真实 REGISTER 401 challenge → Digest REGISTER 200、Keepalive、Catalog 已观察到；API 显示设备 online 和正确 channel。
真实 INVITE 200/ACK 后 media source 建立，video-only RTSP 实际解码 50 帧为 GREEN。
source 为 H264 Main 1920×1080/25fps；轨道还包含 PCMA 8kHz mono，不能用 video-only 结果声称音频解码已通过。
两次摄像机 browser 失败、FFmpeg unsupported-option 与 A/V timeout 的原始结果保留。
source SDP 为 `4D6033`，FFmpeg 识别 Main；但当前 profile parser 的 Main pattern `0xaf/0x00` 不识别 IOP `0x60`，这是更早拒绝点。
此外 source level 5.1 高于 Chrome 153 实际 offer 的 Main `4d001f` level 3.1，是第二项独立协商阻碍。
primary 资料访问未成功，尚未核实规范合法性；不声称 RFC 要求拒绝 `4d60`，列为 `FUTURE_COMPAT_CHECK`，不是 confirmed bug。
正式 Web WHEP 返回 400，无可协商 codec；摄像机浏览器保留 FAIL，多 Chrome 场景 NOT_RUN。
没有修改摄像机 encoding、伪降级 SPS 或修改生产协商逻辑来制造通过。
正式 closeout 证据为 `camera-dahua/final-result.json`，整体 PARTIAL：7 次 Keepalive、3 次独立 INVITE200/ACK 与 3 次 BYE200。
media restart 后 signaling 保留旧 live；manual stop204 → replay 新 ID → RTSP video 再解码 50 帧，符合现有手动恢复语义。
3 个一次性播放 ticket 对应同一 live 且 identity 各异；31 秒后各返回 404，已消费旧 ticket 也返回 404。
最终 stop204、重复 stop404、channel 无 live、RTSP DESCRIBE404，资源释放有实际证据。
GUI 已恢复平台 IP `172.20.63.30`；原 SIP auth 未恢复且用户明确授权，不能声称原平台认证已完整恢复。
两次 media 与 signaling 共 3 个独立 PID 退出码 0，known PID 无残留；相关监听及观测 UDP 端口可重新绑定。
SIP capture 退出码 0，脱敏观测证据保留；owned browser 为空、短目录已移除，私有凭据文件 0600 留给用户，不输出内容。
不能再将“没有任何真实设备”写成原因，也不能将 simulator 当作厂商结果。

## Stage 3：长期运行与资源

短筛 replacement、churn 及混合 soak 均 PASS，observer 退出码 0。
混合测试实际 1818.57 秒、58 source generations；每 wave 21 consumers。
组成是 RTMP/RTSP/WHEP/HLS 各 4、HTTP-FLV 4、GB relay HTTP-FLV 1。
publisher 与 consumers 按 wave 重建；不是同一个 viewer 连续连接 30 分钟。

RSS 前 30 样本中位数 40,140 KiB，后 30 样本中位数 47,190 KiB；settled FD 为 26。
RSS 上升不能单独证明泄漏或证明无泄漏；需结合完整时序、high-water 与 teardown 结果分析。
三项 `owned_processes_after_cleanup=[]`、`cleanup_actions=[]`，没有靠强杀制造正常 teardown。
网络统计按本轮增量解释，不把测试机原有累计 drops/errors 当作本轮故障。
GB soak 的 native harness 返回 PASS，但 observer 因单条 `webrtc dtls failed` 判定 FAIL。
single live/viewer 实际 60.065 秒、视频 framesDecoded 2→1316；10-live multi 实际 1800.049 秒。
GB 资源记录 RSS 28,416→28,148 KiB、PSS 25,583→24,767 KiB、CPU 0.02386 core、FD 46→26；UDP drops 增量 0。
全部 owned processes 最终为空，短浏览器目录经身份核对后删除。
该条日志发生在 single viewer 的 `peer.close()` 附近；原日志未开 debug，不能事后百分之百断言其 SSL error。
持续媒体和资源 cleanup 的 PASS 与原 observer FAIL 分开保留，没有豁免日志或重跑覆盖原结果。

独立 focused close probe 使用原 Release 二进制（SHA `274ab29d67f63ae75a96c891e618eb38a3e0eb91cc038fe6edd7179b349bd1b5`），实际 12.04 秒。
Chrome H264 framesDecoded 3→300，Opus samplesReceived 2400→483360；结果为 `DIAGNOSTIC_REPRODUCED`。
`peer.close()` 后同毫秒出现 debug `peer shutdown`、error `dtls failed` 与 info session shutdown。
该 debug 文案仅来自 `SSL_ERROR_ZERO_RETURN` 分支；两次 DELETE 都为 404，无额外意外错误，cleanup 为 0。
证据位于 `dev-dtls-close-debug/result.json`，强支持原 GB 场景属于相同正常关闭日志误标，但不改写原 GB FAIL。

## Stage 4：性能边界

原计划正式 48 runs；原 non-WHEP 39 次完成，原 WHEP 100 第三次失败，原 supervisor FAIL 保留。
原始 `stage4-supervised/whep-100/whep-100/3/server.log:202` 记录已建立客户端占用 `127.0.0.1:49346`。
服务端最后成功候选为 `49344`，下一媒体池端口 `49346` 被该同机客户端占用，随后三次 bind 返回 `Address already in use`。
native client 构造时先 bind `0.0.0.0:0`，再 POST 创建服务端会话；端口冲突不是已证明的吞吐退化或旧测试泄漏。
bench-only 修正引入显式 UDP bind address，默认仍为 ANY；localhost 性能基线客户端固定 `127.0.0.2`。
独立 resume 完成 WHEP 1/100/250 各三次，supervisor PASS，owned/UDP/listeners 最终为空且 cleanup 无强杀。
派生汇总仅包含原 non-WHEP39 与新 WHEP9；原 WHEP1/100 部分测量排除，不伪造单个 supervisor PASS。
汇总状态 `COMPLETE_WITH_PRESERVED_ORIGINAL_FAIL`，issues 0；48 个独立 PID/starttick 与各 run 的 native thread IDs 匹配。
实际 allocator SHA、mapping device/inode、选定环境、host、server/fixture SHA、workers 与窗口验证完整。
证据 `stage4-combined-analysis.json`，SHA `cbb811642a7f6efa46bb021b4b99077222a047980ccaf9ff960e0a2e1e327ebe`。
生产 Release binary SHA 为 `274ab29d67f63ae75a96c891e618eb38a3e0eb91cc038fe6edd7179b349bd1b5`，不是 final-review 的 O2 binary。
两组原 manifest/summary HEAD 均为 `cc63c0d`；resume 的未提交 bench 修正按文件 SHA 映射到之后的 `998ddbf`，原 JSON 不改写。
新 native client SHA 为 `9b980be9ab08f25d964503b2b506eab63658ba488c482ae111ad3503b6cf5bd1`。
固定 H264 720p30、约 2 Mbps、GOP60/2秒、AAC 48kHz 双声道 128kbps；6 workers、warmup10秒、measurement30秒、每项3次。
下表是三次中位数；逐次 CPU/RSS/PSS/FD/Mbps/first p50/p95/p99/max 及 min/max/sample SD/MAD/CV 保留在汇总 JSON。
PSS 来自 native steady-window 测量，不由 RSS 反推；未提供的 latency percentile 保持 null，不补造数值。

| workload | CPU core | RSS KiB | PSS KiB | FD | Mbps | first p50 ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| publish_only-1 | 0.006 | 20480 | 18403 | 27 | 2.271 | 1763.167 |
| rtsp-1 | 0.015 | 21248 | 18787 | 28 | 2.293 | 1478 |
| rtsp-4 | 0.034 | 22142 | 19205 | 31 | 9.172 | 1439 |
| rtsp-8 | 0.056 | 22992 | 20105 | 35 | 18.343 | 1398 |
| rtmp-1 | 0.014 | 20736 | 18374 | 28 | 2.263 | 1457 |
| rtmp-4 | 0.026 | 21496 | 19215 | 31 | 9.052 | 1417 |
| rtmp-8 | 0.048 | 22780 | 20299 | 35 | 18.103 | 1377 |
| http_flv-1 | 0.016 | 20704 | 18588 | 28 | 2.271 | 1458.393 |
| http_flv-4 | 0.036 | 21248 | 19083 | 31 | 9.086 | 1456.716 |
| http_flv-8 | 0.057 | 21588 | 19501 | 35 | 18.171 | 1457.438 |
| hls-1 | 0.008 | 26368 | 24563 | 27 | 1.749 | 3407 |
| hls-4 | 0.009 | 26624 | 24311 | 27 | 6.996 | 3408 |
| hls-8 | 0.011 | 26624 | 24609 | 27 | 13.992 | 3409 |
| whep-1 | 0.064 | 31488 | 26504 | 28 | 2.327 | 1472 |
| whep-100 | 0.704 | 52820 | 47894 | 127 | 232.704 | 926 |
| whep-250 | 1.617 | 85262 | 80433 | 277 | 581.724 | 1034 |

first-media 是建立延迟，不是端到端解码/显示延迟；native WHEP 是 SRTP 接收/解密/增长，不是 Chrome 解码容量。
未修改生产媒体池或全局临时端口配置；性能优化生产改动为 0。
历史 18,000 路等容量记录不代表当前 HEAD；没有可比 A/B 数据，不宣称不存在性能退化。
不重新开展 shared RTP packetizer、fanout 架构、allocator 或容量上限调整。

## Stage 5：运行包与部署

`bench/package_runtime.py` 打包媒体程序、既有 C++ clients、bench Python、FFmpeg、fixture 和依赖库。
它不自动包含 Go signaling/simulator、Chrome、Playwright、ffprobe 或 tests。
本轮 Go/Python/signaling assets 私有补充与浏览器支持单独记录，未全局安装。
supplement 原 manifest 保留 `72b7bc2` 来源，未伪装成新构建；signaling 自 `a7310e2` 未变化。

runtime-final tar SHA：`0fc913e6e4327a94d9534a54b08e1ed6ef5f89adc86a19cc0dec0d9ac32e3eb2`。
runtime-final manifest SHA：`37c6f1d963595597684cf375c8ebfac66ae26f2f0a428b5a23c7b7f7d8e599e3`。
fixture 为 FFmpeg 生成的 H264 Baseline/AAC，SHA：`2dd7ced4742ea721acc87b4e071b72143957db010ab62f9d0c1b2e3e645cde9d`。
测试机 preflight PASS：429 inventory files、13 private imports、80 ELF dependencies。
该步骤没有启动协议服务；不能代替远端 recovery、WHIP/WHEP 的部署验收。

真实摄像机平台停止后，Stage5 正式一次串行执行结束：supervisor exit0，recovery/WHEP/WHIP 三项均 PASS/exit0。
这是 `TESTHOST_LOCAL_LOOPBACK`，不是新增 CROSS_HOST 证据；跨机证据仍引用 Stage2。
GB recovery 实际 74.08 秒，10 devices/3 lives、6 checks；两次故障注入 exit -9，其余退出码 0，符合预期故障模型。
WHEP 实际 12.44 秒，DTLS connected，framesDecoded 4→305、audio samples 5760→488160。
WHIP 显式 resume 实际 13.42 秒，DTLS connected，framesEncoded 2→242、audio packets 11→611。
其 HTTP-FLV decode exit0、视频 200 帧、A/V timestamp non-monotonic 0；不将编码增长误写成 Chrome 播放解码。
各项 cleanup_actions/owned_after/final_cleanup 均为空，短 browser 目录已移除；56 项 PID/starttick 检查无残留，71 项 scoped TCP/UDP 绑定检查通过。
5 组 expected fault findings 保留，包括故障注入的 BYE timeout/connection refused、正常 DTLS close 的 error 日志和 UDP negative-probe warning。
没有放宽 Stage3 日志 guard 或改写原 GB observer FAIL；预期故障注入不冒充正常网络无错误。
证据为 `stage5-observer/status.json` 与 `stage5-final-attestation.json`；47 文件的 dev/test SHA 逐项核对 PASS。
`stage5-sha256-inventory.json` SHA 为 `29a07779858fcb4c9445abac564a6e5efc199e74a8fa0234d6b98f9560355caf`；
attestation SHA 为 `8c8280e1a5ad50d07341e358cacb88393f085cb2d3ee969b8dacada4c7a39155`。
`build-evidence.tar.gz`、sanitizer-artifacts、runtime tar/manifest 与原始日志保留用于复核。
本轮四个独立中间构建目录 `release`、`ubsan`、`final-review`、`asan` 已按 dev/inode/UID/realpath 核对后删除，
约释放 3.44 GiB；原仓库 `build` 未删除。128 份构建 metadata、7 个代表性 UBSan ELF 与运行包保留。
新版 Release `LastTest.log` 单独保存至 `build-evidence-final-update`，旧归档不覆盖；删除日志为 `build-cleanup.jsonl`。
清理前可读 `/proc` 路径无构建占用，部分非测试进程路径受权限限制，限制原样记录；不宣称检查了全机所有进程。
`bench-bind-build` 是 16 份证据文件，不是构建树，予以保留。摄像机私有测试凭据文件保持仓库外 0600，供后续人工处理认证。
最终只清理本轮明确 owned 资源，不删除原仓库 build 或他人/历史资料。

## 当前交付判断

已完成代表性真实媒体、跨机路径、混合长期运行、fresh 构建与有效 UBSan 回归。
已确认正常 DTLS `close_notify` 经下层 terminal bool 统一映射到上层 error 日志；这是诊断准确性限制。
独立 focused probe 已复现，不新增状态或改变关闭逻辑；后续应单独收口该日志/接口语义。
Stage5 代表性部署路径通过；真实厂商 SIP/RTSP 与生命周期已验证，但该摄像机 Chrome 协商未通过，A/V 音频解码 NOT_VERIFIED。
ASan 为环境阻塞；正常 DTLS close 日志误分类与 `4d60` profile parser 兼容性分别保留为独立诊断/兼容性候选。
摄像机平台 IP 已恢复，原 SIP 认证不能恢复且用户已授权，不能将配置恢复表述为完整恢复。
原 GB observer FAIL 和原性能 supervisor FAIL 保留。
当前结论为有边界的代表性验收：已验证的 supported codec/协议路径可进入受控实际试用，不宣称全部能力或所有设备完全 PASS。
30 分钟级受控测试不是多日稳定性证明，历史 18,000 路等容量不是本 HEAD 的新验收容量，没有同条件 A/B 的无退化结论。
本轮没有证据要求新增生产架构或性能改造；摄像机浏览器目标若是实际部署必需项，需先独立解决可协商编码与 profile 兼容性。
最终 Git、证据核对与清理状态以交付报告为准；仍逐项区分 PASS、FAIL、BLOCKED、NOT_RUN、ACCEPTED_DESIGN。
