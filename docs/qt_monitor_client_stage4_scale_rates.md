# Qt 录像慢速与高倍率呈现补验

## 范围

以 `4f4feb1` 为基线，只扩展桌面回归测试，不改播放器、信令、媒体服务器或依赖。沿用[按输入节奏呈现](qt_monitor_client_stage4_scale_presentation.md)和[主窗口倍率请求](qt_monitor_client_stage4_scale_ui.md)，不把请求值当作设备实际倍率。

本轮 DEV 证据：`/var/tmp/qt-scale-rates-4f4feb1-qgsXsMXQ/`。`predeclared-test-plan.md` 在首轮运行前固定输入、窗口、阈值及截止；首轮失败和后续驱动修正分别保存，不覆盖失败。

## 新增输入及断言

固定八秒 H.264 320×240、25fps、PCMA 8kHz 双轨 PS 的 SHA256 为 `19b8925fe5031cbf35f08c10131b0edf9b00ca466b62937958fb6f8cc3d2dcdd`。重复时仅平移PES的PTS/DTS，媒体负载不变；每行先按1×发送八秒，随后改变真实输入速率十六墙钟秒，最后恢复1×直至完整EOF。Scale 请求在第7秒及第24秒发送，输入调度用独立的有理数整数计算，不读请求值。

| 请求 / 实际速率 | 源时长 | 改速段源末端 | 全部输入及EOF | 预设呈现截止 |
| --- | ---: | ---: | ---: | ---: |
| 0.25 / 0.25 | 64秒 | 12秒 | 76墙钟秒 | 86秒 |
| 0.5 / 0.5 | 64秒 | 16秒 | 72墙钟秒 | 82秒 |
| 4 / 4 | 96秒 | 72秒 | 48墙钟秒 | 106秒 |
| 4 / 3.43 | 96秒 | 62.88秒 | 57.12墙钟秒 | 106秒 |

四行均开启实际SDL dummy队列。原有2×/请求被忽略的静音和有声四行及短尾用例保留。64秒行必须完整呈现1600帧、512000个8k源样本及3072000个48k解码PCM帧；96秒行分别为2400、768000、4608000。逐帧实际paint hash/索引、完整写入字节、同一owner/decoder/唯一GET、两次控制及最终DELETE、零丢弃、非空SDL清空次数为零、自然EOF与最终对象释放均单独检查。

固定16–18、18–22秒检查改速，34–38秒检查恢复1×，不事后寻找通过片段。输入及paint源跨度、估算SDL输出游标和转换后测试音调保留原±10%阈值。只有0.25×慢速段采用两秒子窗：25fps四分之一速每秒仅6/7帧，一秒量化误差本身可达12%，两秒12/13帧则可检验原阈值；这在首轮前已声明。所有恢复1×子窗仍为一秒。

完整PCM累计过零误差预算仍按每份八秒参考最多两周期；重复次数由八份增加到十二份时预算相应为24，不改变每份预算或RMS要求。它不是逐样本等价或宽带音质证明。

## 首轮RED及测试驱动修正

`normal-new-rates-01.log/.exit` 保存首轮：0.25与0.5通过，4与3.43出现节奏断言失败。Qt默认日志上限还截断了后半段QINFO；可以确认失败次数，不能还原被抑制的3.43窗口数值。

首轮4×的16.004–18.003秒，输入约4.002×，paint仅3.402×，估算音频输出游标约0.889墙速。检查DEV实际Qt库反汇编与头文件后，确认长 `QTRY` 使用 `QTest::qWait()`，每轮事件处理后休眠最多10ms。有限播放器每tick最多呈现一帧，生产Scale timer为5ms；测试驱动把4×所需100fps推到了自身轮询上限，也影响10ms音频片段的补给。

独立 `timer-driver-check/` 只测同机5ms PreciseTimer：`qWait` 在3000ms内297次，原生事件循环在实测2850ms内570次，分别约99/200次每秒。后者退出singleShot为默认coarse timer，不能把2850ms当作严格三秒窗口；频率按实际时长计算。

最小修正仅在运行媒体的测试等待中，改用原生 `QEventLoop`、5ms条件检查及Precise截止timer；同一个局部helper有八个实际调用点，栈对象退出即断连。条件尚未满足且剩余预算非正时立即失败，不启动负数timer。初始参数检查和完成后的释放等待仍复用QTest。所有媒体输入、原窗口/阈值、inventory及清理断言不改，生产代码不改。

CTest传入 `-maxwarnings 0` 以保存全部遥测，不压制警告。Scale组因新增约253秒完整输入，将整体上限从330秒预先调整至600秒；各行绝对截止不因运行失败放宽。

`normal-high-rates-02.log/.exit` 原生事件循环复测两条高倍率行：4项QTest通过、零失败/跳过、106.658秒。4×的两个改速总窗口paint为3.98/3.97×；3.43×为3.46/3.42×，两者恢复窗口均约1×，所有原逐秒、PCM及清理断言通过。该候选之后仅追加非正预算检查，最终候选另行验证。

因此首轮RED归于已复现的测试驱动节流，不作为已确认的播放器生产缺陷。不借此调整生产缓存、SWR参数或降低健康标准。

## ASan全套首次中止与Qt内部看门狗

`asan-full-final-01.log/.exit` 及 `.raw.log` 保留完整首跑：9/10个CTest组通过，总707.81秒、退出8，仅Scale组被Qt内部 `Test function timed out` 中止。该进程总计300.041秒，但0.25行当时仅推进约71.7秒，媒体仍前进，未到76秒输入EOF或86秒呈现截止。不能把这次全套写为PASS，也不能称0.25行运行了300秒。

[Qt 6.2.4源码](https://github.com/qt/qtbase/blob/v6.2.4/src/testlib/qtestcase.cpp)的 `defaultTimeout()`（391–399行）支持毫秒环境变量 `QTEST_FUNCTION_TIMEOUT`，默认300000；`WatchDog`（975–1015行）及数据行的begin/end（1115–1120行）表明，看门狗线程可能漏观察相邻数据行之间短暂的结束状态，从而跨行累计等待。这与本次总300秒中止一致，但没有线程调度轨迹，不能声称已经证明本次具体竞态。

失败后只对Scale组增加 `QTEST_FUNCTION_TIMEOUT=600000`，与首跑前预声明的外层CTest 600秒预算对齐；这是失败后补充的runner配置，不声称此内部设置也事先声明。看门狗仍开启，所有每行期限、窗口及完整性断言不变。重配置后检查Scale ASan ELF的SHA256不变，再补跑该组；其他九组的已有结果不冒充第二次全套执行。

## 最终验证

本阶段受控倍率回归PASS，不代表真实相机或七阶段整体验收。

| 验证 | 实际结果 | 原始证据 |
| --- | --- | --- |
| Normal两条高倍率 | PASS，4项QTest、零失败/跳过，106.658秒 | `normal-high-rates-02.log/.exit`，候选之后只补非正预算guard |
| Normal最终两条慢倍率 | PASS，4项QTest、零失败/跳过，149.578秒 | `normal-slow-rates-final-01.log/.exit` |
| Normal/ASan最终构建与重配置 | PASS，均退出0 | `normal-configure-watchdog-01.log/.exit`、`asan-configure-watchdog-01.log/.exit` |
| ASan全套首跑 | FAIL，9/10组，707.81秒、退出8，仅Qt内部watchdog中止Scale | `asan-full-final-01.log/.raw.log/.exit` |
| 修正runner后ASan完整Scale组 | PASS，1/1，11项QTest、零失败/跳过，484.22秒、退出0 | `asan-scale-watchdog-final-01.log/.raw.log/.exit` |

全套首跑中通过的其他九组共463项QTest，加上最终Scale组11项，合计474项零失败/跳过的覆盖，来自两次运行，不是一次全套10/10。最终Scale覆盖原四行、新四行、短尾与init/cleanup；不是只补跑先前中止的慢速行。原始全套FAIL与首轮节流RED均保留。未重跑当前Normal完整十组，也未重新运行无生产改动的Go测试。日志未见ASan、UBSan或LeakSanitizer错误；没有关闭leak detection或使用suppression。

`source-validation.sha256` / `final-validate.sh` 保存Qt内部看门狗修正前的候选及首跑；`source-watchdog-final.sha256` / `watchdog-validate.sh` 保存补齐内部看门狗后的最终候选及定向补跑，不覆盖旧身份清单。Normal先复验原生循环下慢速两行，随后ASan+UBSan+LSan完整桌面CTest，修正后只补跑Scale组。使用既有独立构建和shared FFmpeg9.0.1 SDK，未新建依赖副本。Leak detection保持开启，无suppression。

实际编译器为GCC 16.0.1 experimental，Qt 6.2.4，Normal使用`-O2 -g -DNDEBUG -Werror`，ASan使用`-O1 -g -DNDEBUG -Werror -fsanitize=address,undefined -fno-omit-frame-pointer`。`environment-final.txt`记录编译参数，`runtime-maps.txt`记录实际ASan/UBSan及shared FFmpeg加载；外部Qt、SDL及FFmpeg不是本轮完整插桩的库。

最终CMake/test源SHA256分别为`b3994c2683b5252b00f995777cc8b089f28052decda4e8f945640e69052f8f92`、`bf38606f4615f6e83de6c1215a46839e24aa7743349fba264a8328611e1d9fc6`。Scale ASan ELF为`131b3c0fd23c680835305f4d263286c77f4f99f103d1d3e9c1fa6222f00b813e`，watchdog修正前后完全一致，见`asan-test-before-watchdog-fix.sha256`及`final-elf.sha256`。六份冻结源码最终复核均一致。

首轮候选源码在失败后按原SHA恢复至`candidate-01-recovered/`，明确标记为事后恢复而非事前归档；只保存了该首轮ELF的哈希，不宣称保存了原ELF。当前源码/差异及命令另行保存。独立只读复审未发现本次测试增量的P0/P1，未改任何生产文件或第三方。

## 验证边界

这是固定受控PS、软件paint和数字PCM验证，不是实际相机或物理屏幕/扬声器验收。高倍率完整paint并不保证物理屏幕100Hz。SDL dummy的估算游标不是声卡实测时钟，观测到约1.05墙速也不能宣称硬件48k精确播放；100ms采样不证明每个后端period绝无欠载。

本轮未验证真实Qt→大华Scale、断供恢复、暂停/倍率组合或宽带AAC快速播放品质。保留前文的SWR低通边界，不从440Hz PCMA测试音外推任意宽带音质。七阶段目标及设备决定真实速度/定位精度的边界不变，整个平台仍未GO。

本轮没有新增生产状态、配置、helper、线程或接口。本阶段未删除任何文件；后续清理也只限确认属于我们且已无用途的`/tmp`、`/var/tmp`中间产物，原始失败、日志、素材及身份清单保留，不删除其他Agent或用户资料。
