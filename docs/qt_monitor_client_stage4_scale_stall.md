# Qt 倍率播放中临时断供与恢复回归

## 范围

以 `2cfdc81` 为基线，继[倍率后暂停/继续](qt_monitor_client_stage4_scale_pause.md)，仅扩展 `desktop/tests/scale_playback_view_test.cpp`。本次是停止夹具供给、维持原媒体socket及播放器运行，不发送pause/resume，不将显式暂停成功代替断供恢复。播放器、信令、媒体服务器与第三方均不变。

DEV证据：`/var/tmp/qt-scale-stall-2cfdc81-iTbTlQF6/`。`README.md`为首跑前声明，记录输入、固定窗口、失败分类和期限；原源、预格式化候选、最终 `formatted-candidate.cpp`、命令和日志均保留。

## 固定场景及断言

复用原八份H.264 320×240、25fps和PCMA 8kHz双轨PS，共64源秒、1600视频帧、512000源音频样本和3072000个48k解码PCM帧。八秒原素材SHA256为 `19b8925fe5031cbf35f08c10131b0edf9b00ca466b62937958fb6f8cc3d2dcdd`；八份只平移PES PTS/DTS，总PS 3151664字节，SHA256 `d27303133b0afb57e49f09841ed371b1b9fee5324e3f8a90fdd3b5548da92338`。

新增一条有声数据行 `request-2-actual-2-input-stall-audible`。第7秒Scale2，实际夹具独立按0–8源秒1×、其后持续2×到EOF调度；不再发Scale1。原16–18、18–22秒固定及逐秒窗口仍按原±10%检查输入、paint、估算SDL游标与音调。

采齐22秒样本后、22.25秒前停sender。固定25–26秒断供gate必须先在sender停止时取得26秒端点，且实际停供至少四秒、至多4.25秒；不恢复后才采终点，不搜索较晚窗口。gate要求：

- 两端点存在、各延迟不超过150ms，窗口至少两个样本，不能空集合通过；输入、paint、PCM游标须已有非零进展。
- 窗内逐样本发送源进度、paint索引及估算游标恒定，SDL软件queue为零、状态仍PLAYING，而非暂停用例的PAUSED。
- 未发HTTP终止块，decoder未finished；原owner/decoder/socket仍存活、媒体socket仍Connected，audible且未paused，无错误，GET恰1、API请求仍恰2。后续再次核对原对象identity。

gate不满足明确记为“断供夹具前提失败”，不延长等待或单凭它认定恢复算法有缺陷。恢复时复用既有 `dispatch_offset_us`，以实际停供时长平移剩余包及EOF，不补发欠账突发、不改PTS、不重建媒体。恢复窗口固定34–38秒及各一秒子窗，仍验证真实2×及原阈值。原74秒绝对呈现期限不变。

最终逐帧paint hash和连续索引、全部源样本/解码PCM、零drop、全部PS和bytesWritten、累计数字音调/RMS、零非空SDL清空、SDL排空、自然EOF、DELETE及原对象释放断言均保留。API恰为创建、Scale2、DELETE，共3次，只有一个GET。

## 复杂性与旧行等价性

只新增一个测试数据列和一个测试分支。旧九行 `stall_after_scale=false`，`stream_until_eof`是原pause与新stall的不可变派生值；原九行的输入公式、pause/Scale控制数、恢复窗口、阈值、EOF及截止均等价。实际停供偏移复用原字段，没有新增helper、生产状态、配置、线程、接口或队列。

DEV clang-format 20只格式化新增分支范围，未顺手格式化原有等待helper；`git diff --check`通过，不声称全文件格式检查通过。独立只读审查确认固定gate非空、端点先采齐及原九行等价，未发现P0/P1。

## 定向结果

`validate.sh`记录串行构建及定向命令；每个进程只运行新媒体行加init/cleanup，共3项，不是三条媒体行。`source-validation.sha256`绑定四个构建输入；`test-elf.sha256`保存两个ELF。

| 验证 | 实际结果 | 证据 |
| --- | --- | --- |
| Normal/ASan定向构建 | PASS，分别退出0 | `normal-build-01.*` / `asan-build-01.*` |
| Normal断供行 | PASS，3项，零失败/跳过，40.701秒，退出0 | `normal-input-stall-01.log/.exit` |
| ASan+UBSan+LSan断供行 | PASS，3项，零失败/跳过，40.716秒，退出0 | `asan-input-stall-01.log/.exit` |

两轮gate均为25.000–26.000秒、11个样本，固定发送1802个pack、已发送源进度领先paint 40000µs。Normal/ASan实际停供分别4.000996/4.000986秒；最终媒体行elapsed分别40.225/40.221秒，完整库存、恢复2×及最终资源释放均通过。最大输入调度迟到分别1/3ms，未超过原100ms门槛。

最终测试源SHA256 `8ff1a7a9f45cc2d793f1d5ca739904d0e634afa005373f4e89324939d9cd5ad8`；Normal ELF `d0cd47c48313c854945e4a2d577241fa77d40737313ee1d063b3dcaa0c47376d`；ASan ELF `a21346756c97fb20f40c2ef1a8e95e06c3927d71cf3880387a05f3e8c2feea4a`。

## 完整Scale组

完整组于2026-10-17 20:18:58–20:28:21 UTC串行执行，`asan-full-scale-01.exit=0`，CTest **1/1 PASS，563.50秒**；原始QTest为 **13 passed、0 failed、0 skipped，563435ms**（十条定时行、短尾、init/cleanup）。没有提高现有600秒CTest及600000ms QTest期限。

该轮新增行固定gate仍为25.000–26.000秒、11个样本、1802 pack、40000µs未paint源跨度；实际停供4.000522秒，恢复2×及全部库存/对象释放通过。`asan-full-scale-01.raw.log`未见ASan、UBSan或LeakSanitizer错误。首尾四个源输入SHA均一致，完整测试组已终态而非只轮询到进程存在。此结果仍只是完整Scale组，不是整个桌面或后端CTest全套。

## 验证边界与清理

此gate证明固定数字断供平台，不保证所有内部缓存已彻底耗尽：仍观测到40ms已发但未paint的源跨度，解码器、AVIO/demux及SWR可保留尾部。queue0只表示采样时SDL软件队列空，游标是累计输出减queue及两period的估计，不是SDL后端精确位置、物理静音、断网重连或声学恢复证明。原64源秒库存覆盖也不等于真实设备请求范围库存完整。

环境仍为GCC 16.0.1 experimental、Qt 6.2.4、shared FFmpeg 9.0.1，offscreen/SDL dummy。ASan与UBSan同时启用，LeakSanitizer保持开启、无suppression；定向日志未见三者错误。外部Qt、SDL和FFmpeg不是本轮完整插桩版本。

无生产改动，未重跑无关Go或后端全套。真实相机组合、宽带快速音质、物理声画、第二机16路及完整对讲仍单列边界，七阶段未GO。本阶段复用既有构建/依赖，没有复制新的运行包，也没有删除文件。清理仅限自有、已无用途的`/tmp`及`/var/tmp`中间产物；原始失败、日志、素材、SHA与其他Agent资料保留。
