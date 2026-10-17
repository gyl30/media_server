# Qt 录像倍率与暂停/继续组合回归

## 范围和预声明

以 `f5cb015` 为基线，仅扩展 `desktop/tests/scale_playback_view_test.cpp`，不修改播放器、信令、媒体服务器或依赖。补齐[受控倍率呈现](qt_monitor_client_stage4_scale_rates.md)尚未覆盖的组合：首次Scale已使播放器进入按输入节奏呈现后，暂停和继续能否保留原连接、冻结显示并继续真实2×输入。不是重新验证未请求Scale的普通暂停。

DEV证据：`/var/tmp/qt-scale-pause-f5cb015-MfP5rNSN/`。`README.md`在运行前固定时序、窗口、完整性和期限；`original.cpp`与候选副本保留。`source-candidate.sha256`绑定最终编译源，不能将预格式化副本误称为最终源码。

## 最小测试增量

复用原定时PS、HTTP控制夹具、paint和SDL观察。增加一个数据列及一条有声媒体行 `request-2-actual-2-pause-resume-audible`；旧八条倍率行不改媒体、阈值或期限。

输入仍为固定八秒H.264 320×240、25fps和PCMA 8kHz双轨PS，SHA256 `19b8925fe5031cbf35f08c10131b0edf9b00ca466b62937958fb6f8cc3d2dcdd`，八份仅平移PES PTS/DTS，共64源秒。第7墙钟秒发Scale2；真实输入先1×八秒，之后持续2×至自然EOF，不再发Scale1。

在原16–18、18–22秒固定窗口验证2×，取得22秒样本后、22.250秒前暂停：停止夹具供给，调用真实公开暂停API，响应204并从停止供给起250ms内观察暂停；另给最后一帧250ms完成paint。冻结画面、paint hash/索引及rendered计数，保持至少两秒，期间调用Scale4不得产生HTTP请求。以100ms样本及端点检查SDL_PAUSED、冻结呈现和原decoder存活，不把离散采样写成连续物理声学证明。

暂停时播放器允许向SDL有界预填PCM，因此不要求PCM提交计数或队列绝对不变；检查暂停末尾队列不小于冻结值、采样SDL_PAUSED及显示不推进，不将其扩大为连续队列不可消耗证明。从继续请求发起起250ms内观察受理并恢复；夹具按实际停止供给时长整体平移后续包和EOF，不补发暂停期间欠账突发，不改PTS、原GET或decoder。实际供给平移最多三秒，原74秒绝对呈现期限不放宽。

恢复窗口固定为28–32秒，仍按实际2×及原±10%阈值验证，并检查每个一秒子窗。其余完整断言沿用：1600帧连续索引和逐帧paint hash、512000个8k源样本、3072000个48k解码PCM帧、全部PS字节、零丢弃、零非空SDL清空、真实音调/RMS、最终SDL排空、自然EOF、DELETE及owner/decoder/socket释放。控制恰为创建、Scale2、暂停、继续、DELETE；暂停中的Scale4不计入请求。

唯一新增可变测试时序量是实际供给停止时长。旧八行该值恒零，改速段墙钟末端仍精确为24秒，包括343/100输入；原EOF、固定窗口、阈值和每行绝对期限等价。没有新增生产状态、helper、线程、接口或额外媒体连接。

## 定向验证

复用既有独立Normal和ASan桌面构建，串行执行。`validate.sh`保存实际命令和退出码，固定源SHA后分别构建，再各运行新增pause行与原有声2→1行。`directed-elf.sha256`绑定两可执行程序。

| 验证 | 实际结果 | 原始证据 |
| --- | --- | --- |
| Normal定向构建 | PASS，退出0 | `normal-build-01.log/.exit` |
| ASan定向构建 | PASS，退出0 | `asan-build-01.log/.exit` |
| Normal新增pause和原2→1媒体行 | PASS，4项QTest，零失败/跳过，87.486秒，退出0 | `normal-paused-and-original-01.log/.exit` |
| ASan+UBSan+LSan相同两行 | PASS，4项QTest，零失败/跳过，87.514秒，退出0 | `asan-paused-and-original-01.log/.exit` |

4项包含init/cleanup，不是四条媒体行。Normal实际暂停供给2.022264秒，ASan 2.023445秒；冻结时两者均已呈现895帧。两轮在两秒暂停段各取得20个100ms样本，采样的SDL均为PAUSED、paint/render均冻结。恢复固定窗、完整库存和清理均由上述同一媒体行强制检查，而非只观察控制204。

实际Normal ELF SHA256 `117856efedb02eb3fd71f16fc4c37bd6b312052bed69f23e2022bddb701e5889`；ASan ELF `a419a38c96bd7bfe0e5fddde3ee8d6306b1495e65c9ff6c95aa878daf98d2fab`。最终测试源SHA256 `6057ef7ceed92de75b8ac047d3b6b38219b6350d0531ac5a2146d3ce7be48180`。

## 完整Scale组补跑

`full_scale_asan.sh`在两轮定向退出0后串行执行完整 `desktop_scale_playback_view` 组：PASS，1/1 CTest，12项QTest、零失败/跳过，CTest 522.84秒、QTest 522.788秒，退出0。覆盖原八条倍率行、新pause组合、短尾及init/cleanup，不是整个桌面或后端全套CTest。

原始 `asan-full-scale-01.log/.raw.log/.exit` 保留。该轮pause实际平移2.022038秒、冻结895帧，新增行及所有旧行的完整性/窗口/清理断言通过，日志未见ASan、UBSan或LeakSanitizer错误。测试前后四份源码SHA一致，进程最终退出；不借此继承其他未重跑测试组的历史PASS。

## 环境、审查与边界

GCC 16.0.1 experimental、Qt 6.2.4、shared FFmpeg 9.0.1 SDK，offscreen及SDL dummy。ASan构建同时启用UBSan；保持 `detect_leaks=1:halt_on_error=1`、`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`、LSan退出码23，无suppression。定向日志未见ASan、UBSan或LeakSanitizer错误；外部Qt、SDL和FFmpeg并非本轮全插桩库。

独立只读审查已核对旧八行调度等价、暂停预填语义、唯一GET/owner及完整清理，未发现P0/P1。构建前的全文件clang-format dry-run非零：同时报告原有等待helper及新增排版；后续只格式化新增变更范围，没有顺手更改原有helper。`git diff --check`通过，不声称全文件clang-format检查通过。这不是已复现的生产或媒体回归失败。

本次是受控夹具及数字PCM验证，不是相机暂停/继续或物理屏幕/扬声器验收。真实Qt→大华Scale观察见[实机报告](qt_monitor_client_dahua_qt_scale.md)，不与本夹具合并为相机组合操作通过。断供恢复、宽带快速音质、精确声画同步及完整七阶段仍未GO。未改Go或生产代码，未重跑无关Go测试或整个后端CTest。

本阶段复用原依赖与构建，不复制运行包。本阶段未删除文件；后续清理仅限我们创建、确认无活动引用且已无用途的`/tmp`和`/var/tmp`中间产物。原始日志、失败、SHA清单、素材和其他Agent资料保留。
