# Qt 录像按输入节奏呈现增量

## 范围与边界

本增量从 `5742685` 的[同会话Scale控制入口](qt_monitor_client_stage4_scale_control.md)继续，只扩展桌面播放器。媒体服务器仍转发设备录像，不做中心录像或倍速编码；请求值及HTTP 204都不被当作设备实际倍率或媒体边界。

首次合法Scale操作启用本地队列反馈，此前保留原1×路径。控制拒绝或超时不会撤销反馈：设备可能已执行，也可能忽略，均需由真实输入决定呈现。没有主窗口倍率按钮，本增量不代表完整阶段四或七阶段GO。

## 实现与所有权

- 复用有界解码/GUI队列和既有Qt更新timer；Scale路径改用5ms PreciseTimer以提供4×25fps软件呈现调度空间，不保证物理屏幕100Hz显示。
- 从GUI视频队首所在连续PTS段估计缓冲前沿，不使用可能已跨入下一epoch的decoder统计，也不跨过一秒以上跳变。静音墙钟超过真实前沿时收敛到前沿，避免旧高速时钟在断供后持续跑远。
- 反馈为 `clamp(1 + 16 * (buffered_seconds - 0.12), 0.25, 4)`；采用音频时钟时扣除已排入音频的源时间领先量。该值是局部执行速度，不是无偏的设备倍率测量。解码线程结束后冻结执行速度，完整排空，不丢有限帧或以请求值推算速度。
- GUI持有单个48kHz stereo S16 SWR，将真实PCM按执行速度重采样；这会变调，不是保音高atempo。每次先设置补偿，再查询容量；使用实际返回样本数。复用原PCM队列分段消费，Scale路径按约10ms输出片段喂SDL，没有第二条解码线程或无界发送队列。
- SDL已经排入的旧倍率PCM保留分段的“输出样本位置→真实源PTS”映射，不用当前速度重解释旧数据。暂停冻结输出游标；静音切换保持已经采用的媒体时钟/epoch。实际音频PTS跳变才清除旧音频段，视频跳变不直接清音频。
- 线程结束不等于最终PCM已取走；只有观察线程结束且本tick已取完其最终音频队列，才flush GUI SWR。flush直到实际返回0，不要求残余delay为0；真实源尾映射到实际输出尾。SDL软件队列归零后，以输出样本游标及原period估计处理后端尾部。

新增成员均是输出链路事实：`presentation_rate_`的空值保留未Scale路径；`audio_resampler_`持有滤波资源；`audio_input_end_pts_`记录交给SWR的最后真实输入末端；`audio_spans_`记录旧比例映射；`audio_output_frames_`包含缺失PTS的输出；`audio_clock_frames_`保持暂停/尾部单调游标；`audio_clock_timer_`独立于被视频重锚的墙钟。私有`audio_clock()`由更新、暂停、EOF共用。没有新生命周期状态、HTTP接口、后台任务或生产测试查询接口；stop统一释放。

## 回归方法

证据目录：DEV `/var/tmp/qt-scale-presentation-5742685-3uKOJLJp/`；复用既有Normal与ASan构建及shared FFmpeg9.0.1 SDK，没有更换正常依赖。独立只读审查分别覆盖时钟、异步资源、测试和新增状态必要性。

`scale_playback_view_test`复用从原finite测试提取的私有HTTP夹具，不增加生产抽象。定时发送固定8秒H.264/PCMA素材的八份连续PTS副本，总64源秒、1600视频帧、512000个8k源音频样本、3072000个48k解码PCM帧：

- 实际1→2→1：源0–8秒按1×、8–40秒按2×、40–64秒按1×，完整48墙钟秒输入及EOF；对照行请求2×但始终按真实1×输入，完整64秒。
- 两种输入各有静音、有声行；同一GET、decoder、owner，只有两次Scale及最终一次DELETE，不重建媒体。
- 预设16–18、18–22、34–38秒窗口和其中每个1秒子窗；真实dispatch源跨度及实际paint源跨度均须在目标±10%，不搜索事后通过窗口。完整1600帧实际paint hash、连续索引、双轨解码数、零丢弃及自然EOF后对象析构分别断言。
- 有声行对真实SDL调用作测试target限定的透明linker包装，累计实际提交PCM的周期/RMS；固定窗口检查转换后音调和估算输出游标推进。没有替换音频后端或改变生产控制。累计周期/RMS不是逐样本内容等价或声学证明。
- 发送timer与GUI共事件循环，记录dispatch lateness、bytesWritten和待写字节；最大迟到须≤100ms。它不是独立网络到达时钟。音频后端是SDL dummy，游标为软件队列加双period的估计，100ms队列采样不能证明每个后端周期绝无欠载。

短尾用例另用同SDK支持的Matroska/H.264+48k PCMA：50视频帧、仅32源音频帧，透明SWR包装只统计GUI线程。明确要求普通转换输出0、EOF flush非零、全部输出排入SDL、自然排空和资源析构；这不是PS或设备格式兼容验收。初次NUT探针因SDK未启用该demuxer失败，原始失败保留，没有扩大生产格式支持。

既有finite测试追加Scale后前后PTS跳变、双轨epoch分离及切换静音，保留全部帧hash/PCM/清理断言。旧一次性burst用例的固定1×耗时下限与真实输入节奏无关，候选版本已复现该单一断言失败；仅删除其不再适用的耗时下限，以独立定时1×对照检查“请求被忽略”，没有删帧/减少媒体量。原非Scale时序下限保持不变。

## 验证记录

最终六份源码固定为`source-validation.sha256`，完整运行命令在`validate.sh`。Normal与ASan采用同一套输入；Normal为`-O2 -g -DNDEBUG -Werror`，ASan+UBSan为`-O1 -g -DNDEBUG -Werror -fsanitize=address,undefined -fno-omit-frame-pointer`并保留LSan，无抑制；外部Qt、SDL和shared FFmpeg库不是完整插桩版本。

| 验证 | 当前结果 | 原始证据 |
| --- | --- | --- |
| Normal完整构建 | PASS，退出0 | `normal-build-final-01.log/.exit` |
| Normal完整CTest | PASS，10/10，469项QTest零失败/跳过，631.98秒 | `normal-full-01.log/.raw.log/.exit` |
| ASan完整构建 | PASS，退出0 | `asan-build-final-01.log/.exit` |
| ASan+UBSan+LSan完整CTest | PASS，10/10，469项QTest零失败/跳过，636.05秒，退出0 | `asan-full-01.log/.raw.log/.exit` |

前置静音两行完整定时测试`muted-pacing-01.log`通过，113.471秒；前置有声两行`audible-pacing-01.log`通过，113.579秒。这些属于其各自候选输入，不替代上述最终源码全套。短尾Qt定向`short-tail-green-01.log`共3项通过、904ms，实际normal输出0、flush输出8、SDL排入8。短尾SDK独立探针`.25/1/4`均普通输出0，flush分别124/31/8后返回0；不把该局部固定SWR实验当作播放器全倍率动态通过。

候选开发过程保留原始失败：`epoch-clamp-red-01.log`稳定复现向前PTS跳变被旧shown-end压回，导致EOF不能完成；修正从当前队首建立连续段后，`epoch-clamp-green-01.log`五个定向数据行通过（加init/cleanup共7项，11.532秒）。未声称这是旧已提交版本的缺陷。

## 仍未验收

本轮不将2×受控HTTP成功外推到所有倍率、真实大华、物理声音或精确控制落点。慢倍速、4×/非整数实际速率、断供恢复、暂停与倍率组合、主窗口按钮和Qt→真实相机双轨Scale仍须完成。既有实机SIP/输入取证仅作为设计依据，不继承为本增量播放PASS。

后续增量已接通[主窗口倍率请求](qt_monitor_client_stage4_scale_ui.md)，并完成[0.25/0.5/4/3.43受控呈现补验](qt_monitor_client_stage4_scale_rates.md)；各次源码、失败和测试范围独立记录，不回写本报告的历史469项结果。真实相机、断供恢复、暂停组合和物理音视频边界仍保留。

未实测首批音频缺失PTS、其后才出现首个有效PTS的混合输入；首次有效映射之前的同步属于best-effort，不承诺未知前缀的精确源时间。本轮没有为这一未复现场景增加状态或接口。

宽带音频品质另有源码边界：实际SDK对应FFmpeg9.0.1的`libswresample/resample.c:188–195`仅按名义采样率初始化低通；`:280–344`补偿只扩相位表/更改步长，不按实际加速比重设截止。48k同rate初始化再加速至4×时，不能据此保证原6kHz以上内容的抗混叠。PCMA8k的原有效带宽至多4kHz，不能用该宽带反例直接宣称本轮PCMA用例失败；但也不能从440Hz测试音通过推导宽带AAC快速播放品质通过。本轮仅源码审查，未量化实际AAC失真或SNR，保留为后续品质验证限制，不改未Scale的既有AAC路径，不静音或缩减输入来获取通过。

只清理`/tmp`、`/var/tmp`内确认属于本轮且无需保留的临时产物；原始失败、媒体、源码/程序身份及报告不自动删除，其他Agent资产不动。
