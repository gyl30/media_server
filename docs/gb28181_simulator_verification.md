# GB28181 simulator 系统验证

## 验证范围与状态

BASE_HEAD：`ab236fcbef8e624930723c37e46fd1bfc3e4dc7b`。

生产二进制源码：`112d7a4d0fb2ff383b76c182cb618ffd29ec3841`。后续阶段提交仅修改测试脚本；二进制 SHA256 与构建机逐项核对。最终文档提交通过 `git log -1 --format=%H -- docs/gb28181_simulator_verification.md` 定位，避免循环引用自身 SHA。

当前是阶段性记录，尚未完成整个验证：基线、correctness、control 与 media 已验收，并记录 burst 与测试宿主 fd 边界；recovery、长期 soak 尚未验收。未完成阶段不计入通过结果。

仅验证本产品与自有 simulator，**own-product simulator verification only**，不证明厂商互操作性，也不推导产品容量上限。控制面设备注册规模与媒体面同时 live 规模分开报告。

结构化证据见 [gb28181_simulator.json](verification_results/gb28181_simulator.json)。完整命令、进程退出码、原始日志与采样保留在测试机 `/tmp/media-test-112d7a4-static`；JSON 记录原始结果路径和哈希。

## 环境与部署

构建机为 `gyl@172.20.45.187`，测试通过该机 SSH 到 `root@172.20.63.32`。测试机 hostname 为 `seclead-Super-Server`，Ubuntu 22.04.5，kernel 6.8.0-84-generic，32 CPU，MemTotal 98,504,064 KiB。不得将构建机上的早期验证混作测试机验收。

Fresh RelWithDebInfo 位于 `/tmp/media-test-112d7a4-static`，实际编译包含 `-O2 -g -DNDEBUG -Werror`，主二进制有 `.debug_info`，未 stripped。仅通过 CMake 选项选择 Boost 1.92 静态库及静态优先依赖，没有修改 CMake 源码。仍有动态依赖，随二进制放在测试目录的 `lib` 中，通过 `LD_LIBRARY_PATH` 使用，未替换系统库；不能称为全静态二进制。

```sh
PKG_CONFIG_PATH=/home/gyl/ffmpeg901/lib/pkgconfig:/tmp/libsrtp-2.7-prefix/lib/pkgconfig \
cmake -S . -B /tmp/media-test-112d7a4-static \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DBoost_DIR=/usr/local/lib/cmake/Boost-1.92.0 \
    -DBoost_USE_STATIC_LIBS=ON \
    -DOPENSSL_USE_STATIC_LIBS=TRUE \
    -DPKG_CONFIG_ARGN=--static \
    -Dpkgcfg_lib_SRTP_srtp2=/tmp/libsrtp-2.7-prefix/lib/libsrtp2.a \
    -DMEDIA_SERVER_TEST_FFMPEG=/home/gyl/bin/ffmpeg \
    -DMEDIA_SERVER_BUILD_BENCHMARKS=ON
cmake --build /tmp/media-test-112d7a4-static -j12
```

Chrome 153.0.8010.52，Python 3.10.12，Playwright 1.58.0，aiohttp 3.11.16，FFmpeg 7.1.1。视频由 FFmpeg lavfi 生成；没有搜索全局目录寻找素材。simulator fixture 为带 AUD 的 H264 video-only；音频公共协议回归使用独立生成的音视频 fixture。

## 现有职责与测试入口

simulator 只模拟设备，执行 SIP REGISTER、Catalog、Keepalive、INVITE/ACK/BYE 与 RTP/PS，不调用设备管理或 play/stop API。`bench/gb_simulator_verify.py` 通过正式 HTTP API 预置 allowlist、播放、停止、删除和故障编排，读取 `/proc`、simulator summary、Chrome getStats 与日志。没有新增生产 debug API、测试框架或 simulator 内部控制面。

脚本按 `--stage correctness/control/media/recovery/soak` 运行，每次 `--output` 必须是新目录。默认用固定 seed 42，普通函数组织各场景。CTest、E2E 和长期测试串行执行，不能同时占用同一媒体端口池。

测试机上的阶段命令形式如下，实际每次命令保存在对应 `result.json` 中：

```sh
export LD_LIBRARY_PATH=/tmp/media-test-112d7a4-static/lib
export PYTHONPATH=/tmp/media-test-112d7a4-static/python-deps
python3 /tmp/media-test-112d7a4-static/bench/gb_simulator_verify.py \
    --stage correctness \
    --media /tmp/media-test-112d7a4-static/media_server \
    --signaling /tmp/media-test-112d7a4-static/signaling \
    --simulator /tmp/media-test-112d7a4-static/simulator \
    --ffmpeg /tmp/media-test-112d7a4-static/ffmpeg \
    --browser /tmp/media-test-112d7a4-static/chrome/chrome \
    --output /tmp/media-test-112d7a4-static/gb-correctness \
    --port-base 44420
```

## 基线回归

| 验证 | 实际结果 | 原始证据 |
| --- | --- | --- |
| Go test/vet/race | 两个包 test/race 通过；vet 退出 0 | `go-test.log`、`go-vet.log`、`go-race.log` |
| `cmake --build build -j12` 与 fresh build | 通过 | `normal-build.log`、`build.log` |
| 测试机 CTest | 14/14，23.35 秒，串行 | `ctest-test-machine.log` |
| GB signaling E2E | 14 项，54.25 秒 | `gb-baseline/result.json` |
| Web UI E2E | 24 项，84.58 秒，pageerror 0 | `web-baseline-green/result.json` |
| 公共协议 smoke | RTMP/RTSP/WHIP 各 5 波，全部推进 | `runtime-smoke/result.json` |

CTest 使用复制的测试二进制与脚本，仅调整测试机临时生成的路径；未修改仓库 CMake 或测试源码。公共 smoke 每个输入均测 1/4 viewer 及 GB UDP、TCP sender active/passive 中继，输出 RTMP、RTSP、WHEP、HTTP-FLV，直接输入波另测 HLS。WHEP runtime failure 与 SRTP unprotect failure 均为 0，并使用独立 FFmpeg 解码验证。

GB 基线三个 Chrome viewer 的 framesDecoded 分别从 52/27/2 增长到 121/96/71，均 ICE/DTLS connected。Web RTSP 源路径覆盖 H264+AAC 输入及共享 AAC→Opus WHEP，视频 2→53 帧、Opus samples 1,920→98,880；不改变共享转码架构。

## Correctness 与生命周期

完整阶段在测试机运行 352.03 秒，24 项检查通过，306 个子进程全部按预期退出，没有异常退出。该次 harness SHA256 为 `2d68188b318c398b7aff4aa10486a7270d5c80a0674b73e9d01c32f4862d243b`，与 `16cc306` 提交一致；结果记录的 HEAD 为测试时的 `c8884bd`，不能凭该字段声称测试了其他生产二进制。

| 场景 | 实际覆盖与结果 |
| --- | --- |
| 单设备完整生命周期 | 100 轮；每 10 轮真实 Chrome decode，其余真实 FLV media；每轮注册、播放、关闭 viewer/复用、停止、重播、Expires:0、重注册、删除、403 拒绝均执行 |
| 清理 | 每轮 ticket 不可再消费、receiver delete 返回 404、设备 DB row 为 0、media UDP sockets 归还 |
| play/stop churn | 100/500/1000 轮；live identity 不复用，receiver/ticket 均清理；中位耗时分别 7.04/7.11/7.13 ms |
| 同通道并发 play | 8/16/32/64 请求；同一 live、N 个不同 ticket，仅增加一次 INVITE/ACK |
| ticket 并发消费 | 100 与 1000 轮；每轮两个同时 POST，始终一个 201、一个 404，成功资源随后删除 |
| ticket 过期 | 100 与 1000 张，各等 31 秒；旧 ticket 全部 404，新 ticket 可消费，live 保留 |
| delete/play race | 100 轮：94 次 404、6 次 502 `context canceled`；设备删除后无 orphan live/ticket/receiver |
| stop/replay race | 100 轮：64 次 409、36 次 201；旧 live delete 为 404，不删除新 generation |

单轮完整生命周期中位 2.466 秒，前 10 轮均值 2.451 秒，末 10 轮均值 2.473 秒。这是短生命周期验证，不替代长期资源趋势验收。

## Control 完整验证与容量边界

完整 control 在测试机运行 2799.45 秒，21 项检查通过，结果为 `PASS_WITH_CAPACITY_BOUNDARY`。61 个子进程均按预期退出：40 个正常退出 0，burst fixture 一个退出 1，20 个故意 SIGKILL；异常退出为 0。原始结果 SHA256 为 `db3a019a2c08a467fd2f2d2f3552864e0785de4963114a4a906a165f386189e1`。

| 子项 | 实际结果 |
| --- | --- |
| 注册阶梯 | 10/100/500/1000/2000/5000/10000 设备均完成 REGISTER、Catalog、10 秒短稳态及 Expires:0 清理；rate 200/s、endpoint 4 |
| 10000 设备预置与注册 | 正式单设备 API allowlist 60.39 秒，REGISTER/Catalog 60.42 秒；不使用 batch API 或旁路 |
| 1000 设备注册速率 | 50/100/200/500/1000/s 均通过；各自稳态 register_fail/heartbeat_fail 为 0 |
| 1000 设备 burst | REGISTER 全部成功，Catalog 有界队列拒绝 613 个任务，未全部同步；simulator 按预期退出 1，随后 offline/channels/media sockets 清理，记为 CAPACITY_BOUNDARY，不记作全成功 |
| SIP endpoint 分片 | 同样 1000 设备，1/4/16 endpoint 均完成注册、Catalog、稳态和注销 |
| 100 设备 keepalive/refresh | 600.12 秒、61 样本，expires 30 秒；Catalog 恒为 100，refresh 3700，heartbeat_ok 12080、heartbeat_fail 0；随后正常注销清理 |
| 500 设备 keepalive/refresh | 600.13 秒、61 样本；Catalog 恒为 500，refresh 18500，heartbeat_ok 60401、heartbeat_fail 0 |
| 1000 设备 keepalive/refresh | 600.12 秒、61 样本；Catalog 恒为 1000，refresh 37000，heartbeat_ok 121000、heartbeat_fail 0 |
| offline/re-register | 100/500 设备各 20 轮，正常 Expires:0 与 SIGKILL 交替；每轮等待 offline 与 channels 清空，再用相同 identities 重注册，清理 media UDP sockets |

三档长窗口中 media/signaling/simulator 的 fd 分别保持 26/8/41，socket 保持 3/2/36，UDP drops 为 0；media RSS 恒为 18432 KiB。100 设备时 signaling RSS 从先前 10000 设备阶段的高水位回落；500/1000 设备 signaling RSS 初末分别为 86988→108852、127212→148780 KiB，simulator 分别为 17288→15192、23796→17164 KiB。未把初期预热或 allocator/GC 高水位直接判为泄漏。初末、最小/最大、中位数、CPU、RSS 与 0/5/10 分钟采样在 JSON 中记录。

三档稳态 heartbeat_fail 均为 0；正常群体注销后 final summary 的 heartbeat_fail 分别为 0/135/531，来自已 offline 设备的并行 heartbeat 被拒绝，与稳态分开记录。完整 control 日志扫描仅发现 burst 的 613 条 Catalog queue full 与对应 simulator Catalog 超时，未出现其他匹配项。这里不把控制面注册规模等同于媒体同时 live 容量。

## Media、recovery 与长期 soak

media 已完成两次运行组合覆盖；recovery 首次运行在 SIGKILL 清理断言处失败，正在定向复查，长期 soak 尚未运行。仍必须完成故障恢复、至少 30 分钟单 live/viewer 和 20 分钟多 live 才能结束整个 Goal。不能以运行中默认 `FAIL` 状态文件或一次采样判定任务终止。

首次 media 阶段在 956.95 秒后因 500 路创建失败终止；此前 1/10/25/50/100/200 路均完成逐流前后真实 FLV 读取、10 秒稳态及清理。各档 media fd 为 28/46/76/126/226/426，停止后均回到 26，media UDP sockets 清零。该失败结果保留为 `gb-media-green/result.json`，没有覆盖为 PASS。

定向 `strace` 复现证明测试 shell 的 `RLIMIT_NOFILE=1024` 是失败原因：创建 498 个 UDP receiver 后，下一次 `socket()` 返回 `EMFILE`。每条 receiver 使用 RTP/RTCP 两个 fd；相同二进制仅在测试子进程提高到 8192 后，500 个 receiver 全部创建成功，fd 为 1026。两次 probe 删除 receiver 后 fd 均回到 26、UDP sockets 清零，再创建成功，服务正常退出。证据为 `fd-boundary-red/result.json` 与 `fd-boundary-green/result.json`；这是测试宿主资源边界，不是产品媒体端口池硬上限，也不是媒体进程崩溃。

后续通过测试 shell `ulimit -n 8192` 继续 500 路及尚未完成的 loss/profile/worker 场景，结果保存在新的 `gb-media-fd-green`；较低档位保留首次运行的真实证据，不要求重跑来抹去 RED。harness 现在记录实际 `rlimit_nofile`，未修改系统 limits、媒体端口范围或 queue capacity。恢复、soak、限速注销定向复验与最终 CTest 将在同一 runner 中串行执行。

该次 media 续跑完整结束，16 项检查通过，1444.79 秒，16 个子进程均正常退出 0；与原运行已完成的六档较低 live 检查组成 22 项覆盖。结果 SHA256 为 `28d1a3afc927ee740f5c80ca6abac7186e76b6b33e661e6fd1549fb78637aec3`；harness SHA256 为 `b4b034fb183bc45ee388918d0aa2f6c5454d82d754e03fbe9def58e4332fa3ff`。

| Media 场景 | 实际结果 |
| --- | --- |
| 真实 Chrome 1/3/8/16 viewer | 每个 viewer framesDecoded/bytesReceived 均增长，始终只有一次 INVITE/ACK、一路 upstream；16 viewer 初末分别为 377→424 至 4→51 帧 |
| 500 active live | 每条流前后读取 4096 字节真实 FLV；10.09 秒稳态，500 INVITE/ACK、500 live，send_errors/phase_drops/UDP drops 为 0；累计 RTP 41286880 包、49197363152 字节，最后 500 BYE |
| 500 live 资源与清理 | 稳态 media fd 1026/socket 1003、RSS 156464→156124 KiB、CPU 0.966 核；停止后 fd 26/socket 3、UDP sockets 清零、RSS 145376 KiB；旧 ticket 与 receiver 均不可再用 |
| packet loss seed 42 | 0/1/5/10% 下 Chrome 最终 decoded 为 450/416/184/77，ICE/DTLS connected；100% 不建 viewer、不要求解码，RTP sent 为 0，stop/receiver/port cleanup 通过 |
| normal/high profile | 直接使用 simulator 的既有 FFmpeg profile；各 10 秒窗 decoded 3→234、3→235，send_errors/phase_drops 为 0 |
| simulator worker 组合 | 默认 16/16、1/1、32/32；各 10 路 live 前后真实读取、稳态和清理通过 |
| 批量 Expires:0 | 10 路活跃媒体注销后 ticket/receiver/ports 清理，再注册与播放使用新的 live identity，媒体正常推进 |

500 路稳态 heartbeat_fail 为 0，注销完成后的 final summary 为 84，属于并行 heartbeat 在 Expires:0 后被拒绝，不混入稳态。media 日志匹配 34 条全部为真实 Chrome 主动关闭后的 DTLS close_notify 终止，无其他匹配项。500 路仅作短稳态及生命周期验证，不替代尚未执行的 100 路长期 soak，也不定义最大容量。

新增环境记录后，构建机再次完成 Go test/vet/race、普通与 fresh build 目录构建，日志为 `go-test-fd.log`、`go-vet-fd.log`、`go-race-fd.log`、`normal-build-fd.log`、`build-fd.log`；二进制哈希未变。恢复阶段尚未 GREEN，最终 CTest 未执行，不提前验收。

## 已发现问题与分类

1. **simulator 问题**：旧固定 30 秒 unregister context 不够覆盖低速群体 Expires:0，32 设备、1/s 退出曾留下 2 个在线设备。修复仅按设备数/实际限速延长退出 context，不改 signaling 或 C++。构建机定向 GREEN 已通过；与测试机正式结果分开记录。
2. **harness URI 问题**：测试机 Web RED 在直接 RTSP WHEP 的相对 `Location` 上错误连接端口 80。使用 `urljoin(response.url, location)` 后完整 24 项 GREEN；不修改产品路由或 WHEP resource。
3. **harness 时序问题**：设备 API 按 expiresAt 即时显示 offline，channels 在每秒 expiry sweep 清理。100 设备 SIGKILL 三次观测，offline 到清空延迟为 0.524/0.622/0.638 秒。`offline()` 分别等待两个真实最终条件，三次 GREEN；未通过延长产品 timeout 掩盖清理。
4. **harness 断言范围问题**：控制 RED 的 5000 设备全部 registered/Catalog，无 live/RTP/send error，但空 media worker phase 一次未调度产生 `phase_drops=1`。源码确认该计数也包含没有活跃 session 的空 phase。仅在 active-media 阶段要求 phase_drops 为 0；control 保留原始计数且明确要求零 RTP。修正后完整 control 21 项 GREEN，并单独保留 burst 容量边界。该次 harness SHA256 为 `196b452ebc9c81ba331e0e6f1db0f2a42b9ebbb7c36adce7e0f7d843316bdc8a`。
5. **测试宿主资源边界**：原 SSH shell 的 fd 软上限 1024，500 路创建时出现 `EMFILE`。定向 RED/GREEN 证明原因并验证清理；只调整测试子进程的限额并记录，不修改产品代码，不将原失败伪装成全通过。

日志错误按具体时序分类：设备删除后的 403 与故障注入退出是预期；delete/play 的取消返回已验证资源清理。Chrome 主动 peer.close 可产生 DTLS close_notify，底层 `SSL_ERROR_ZERO_RETURN` 被现有 session 记录为 `dtls failed` 并关闭 viewer，这不是媒体进程崩溃。不仅按字符串给所有日志一律 PASS，也不为了消除日志修改生产代码。

正常群体 Expires:0 注销期间，已经 offline 的设备仍可能有并行 heartbeat 收到 403。稳态检查与 teardown 的 final summary 分开报告，不把 teardown heartbeat_fail 冒充稳态失败，也不删计数美化结果。

## 生产修改、Sanitizer 与边界

相对 BASE_HEAD，C++、Go signaling server、Web production、`third/**` 均没有修改；唯一非 harness 修改是 simulator 的限速注销 context。没有容量调参、自动恢复 live、新 debug API、history replay 或上游关键帧请求。

本 Goal 允许 C++ production 为 0 时不重复 ASan/UBSan，因此本轮未执行，不能写 `UBSAN_REPORTS=0` 冒充运行结果。未运行 TSan，也未绕过 Boost.Context guard。

仍遵守现有 MVP：先 allowlist 才 REGISTER、持久设备与 runtime 分离、没有自动恢复旧 live、没有 viewer refcount/idle auto-stop。simulator 与产品共用实现假设，本结果不是外部厂商设备兼容报告。
