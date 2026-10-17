# Qt 同会话录像定位增量

## 范围与状态

后续更新（2026-10-18）：本页保留`582bf95`实现与受控回归的原始范围。当前Qt播放组件经生产Go/API/SIP、真实大华及生产媒体转发的限定seek组合已补验，Normal与ASan各14/14；详见[实机增量与边界](qt_monitor_client_dahua_qt_seek.md)。主窗口鼠标操作、物理声画、精确落点及Scale仍未完成，以下历史NOT_RUN不追改为当时已执行。

本增量接续 `f4c19dc` 和 `ae6f99d` 的 Range/Scale 信令接口，仅接上 **Qt 同会话 seek** 和实际时间戳跳变的有限呈现。Scale 的客户端音视频适配仍未完成；实际摄像机经新 Qt 操作的端到端定位、本地物理屏幕和扬声器验证未执行，不能据本节宣布阶段四或整个平台 GO。

设备仍决定实际落点和精度。HTTP 204 仅表明设备受理，客户端不显示伪造的实际进度，不以响应完成后的首帧划定新段。已有大华取证表明新媒体可能早于 INFO 完成，且不提供 RTP-Info；详见[原始取证与离线补证](qt_monitor_client_dahua_playback_controls.md)。

## 接入与所有权

- 主窗口增加原录像 UTC 范围、相对原起点秒数输入（三位小数）和显式“请求定位”。修改数字不自动发请求；切换画格按该格原范围更新并归零。
- `playback_view` 仅保存原请求的起止时间，不能从当前显示 PTS 或包含额外宽限的 owner deadline 推导该范围。创建成功前可展示范围，但无操作权限。
- `seek_record()` 只在原历史 owner/decoder 存在、已有呈现、decoder 未结束、未暂停且无在途控制时开放；pause/resume 原有 EOF 尾部控制行为不变。位置需有限且位于闭区间 `[0, end-start]`。
- 使用原 `record_owner_`、`record_id_`、`session_` 与 `request_`，向原平台 POST `{"position":数值}`。8 秒客户端期限不改变 Go 的 5 秒设备总期限。没有第二个 HTTP client、会话注册表、重试或 pending-seek 状态。
- 409/502/超时提示未确认，不发送反向控制；404/实例已结束沿现 stop 回收。stop 先销毁原 QObject context，再取消请求；切平台后的旧受理响应不能作用于新画格或新实例。
- 请求和 204 都不丢弃本地已解码队列、不清 AVIO/raw HTTP/PS、不重开 GET/票据/INVITE，不执行 codec flush。设备忽略定位时，原媒体仍应完整呈现。

## 实际时间轴的 best-effort 呈现

有限回放维持原有视频/PCM 容量、背压、普通逐帧显示及正常 EOF 排空：

1. 视频队首相对上一已呈现帧的结束 PTS 出现超过 1 秒的正/负 gap 时，就地重锚现有 wall clock。它是本地大跳变同步策略，不是业务 seek 成功检测或精确边界。
2. 音频复用原 SDL 连续性检查：真实 block PTS 相对上一音频结束 PTS 超过 100 毫秒时清旧 SDL 数据，但保留音频设备和 audible 选择。已建立的视频 wall 不被先跳变的音频抢写。
3. 音视频先后进入新段、时钟相差超过 1 秒时，暂用视频 wall，直到实际轨道重新接近。这样音频先回跳时旧视频不会等待 19 秒，视频先跳时也不被旧音频拉回。不拿一轨 PTS 当另一轨的精确归属证明。
4. 解码 worker 在实际音频 gap 处，仅重新初始化同参数 SWR，避免上一段极短滤波尾部拼入下一段；有旧 delay 时计一次 `audio_dropped`。不刷新 H.264 codec，普通 EOF 仍 drain SWR。
5. 若两轨最终仍相差超过1秒，仅在 decoder 已结束、最后队列已提取、视频/PCM队列为空且末帧时长已履行后，退役已完成的视频尾部等待，将尾部时钟交还真实音频。保留 SDL 尚排队的数据及两个后端 period；不改音频实际 PTS，不新增完成标志，不让视频时间轴额外等待另一个时间轴的20余秒。

允许旧数据短暂过渡，不承诺设备或网络中旧数据在固定毫秒内消失。跨段 SDL 已排队旧声音会被主动舍弃，SWR 尾部也会丢弃，不能宣传跨定位全部样本无损；普通连续录像的完整性断言不放宽。没有新增关闭、播放或跳变状态，也未增加 buffer 容量。

## 可复核测试与原始失败

证据位于 DEV `/var/tmp/qt-seek-f4c19dc-6r7jDUHF/`，不是实际 NVR 的新验收：

- `red-build.log`、`red-clock.log`：未改生产代码时，真实 H.264/PCMA PS 在同一 GET 中按原段、PTS/DTS 平移 20 秒的新段、原段组合。四种音视频先后顺序/静音组合均失败，实际约22–26秒，超过相同11秒呈现门槛。这是时钟问题，不是解码失败。
- `green-directed-01.log`：时钟四组已经通过；控制用例错误地读取瞬时状态文字，被正常视频刷新覆盖，随后失败清理没有充分派发 Qt finished/deleteLater 导致后续测试连带失败。记录保留，不当作新的生产缺陷或掩盖失败。
- `green-directed-02.log`：改为观察实际状态信号，并使测试失败回收等待 QObject 真正析构。12 项 QTest 全通过，0失败/跳过，47.136秒。前后跳变各组150帧全序列图像 SHA 一致、无视频丢帧，生成287808个48kHz stereo PCM样本；两次 SWR 旧尾部舍弃共192个样本，`audio_dropped=2`。静音约5.98秒、SDL dummy有声约5.52秒，差异包含跨段清旧SDL数据，不能称无损音频或物理听音。
- 有声行检查无 `playback_error`、活跃期间不发生 `audible_changed(false)`，防止静音降级假通过。
- 控制测试检查0/小数/范围上界、无效/非有限位置零请求、在途第二次seek/pause拒绝、204但媒体不变仍200帧/PCM对照完整、409/502/超时未确认、404结束及精确 DELETE。409/502分支不宣称其后完整200帧已验收。
- 固定原平台用例：修改公共 client 平台后仍向原 owner 控制；在途 stop/新播放与原204竞态，不修改新状态。原/新实例各自 GET 和精确 DELETE；不要求已取消的客户端真的收到原204。
- `monitor_window_test` 另验原范围、创建中禁用、显式确认、选择不同格、busy/暂停/stop禁用与非选中格响应隔离。实际媒体启用用例依赖原私有 PS，缺环境变量会明确 QSKIP。
- `normal-full-01.log` 首轮全量为8/9，两个新UI用例失败：主窗口首次异步平台加载会按原语义停止旧画面，测试未等待加载完成便启动录像；随后 `ui-directed-01.log` 暴露手写响应为 `video/mp2p`，不符合现有有限入口严格要求的 `video/mpeg`。仅补既有设备树就绪屏障、修正测试响应，未改变产品关闭或媒体类型校验。`ui-directed-02.log` 两用例及初始化/清理共4项通过，0跳过、799毫秒；原全量和定向失败输出均保留。
- `normal-full-02.log` 首次修正夹具后的完整桌面CTest为9/9、357.12秒，原始QTest输出共445项通过、0跳过。它发生在下面额外尾部修复之前，不冒充最终源码的全量验收。
- 独立复审另找出单轨回跳后的尾部跨时间轴等待：`tail-red-01.log` 中视频单独回跳、有声行的100帧/hash/192000 PCM已先通过，但 decoder 未能在2秒内释放。另一次失败是该失败后的测试兜底回收派发不完整，不是第二种生产缺陷。视频回跳静音、音频回跳静音/有声三个对照已经通过。随后只修正上述EOF交钟；测试兜底在独立析构断言之后显式回收已结束线程，不能把兜底删除当作成功断言。
- `tail-green-01.log` 四种单轨回跳/静音组合均通过，含初始化/清理为6项、0失败/跳过、16.899秒。每行100帧全序列hash一致、无视频丢帧；视频单独回跳保留192000 PCM样本、音频丢弃计数0，音频单独回跳保留191904样本且仅计一次旧SWR尾部丢弃。各行实际3.948–4.052秒，decoder在同一2秒断言内析构，仍精确GET/DELETE各一次；有声行无错误或活跃静音降级。保留真实PS的DTS回跳诊断，不以日志完全无警告作为通过条件。

测试仅平移现成合法编码数据的 PES PTS/DTS，SCR 不参与这些呈现测试；不将合成输入称作完整 PS 协议合规测试、真实设备、或 seek POST 与真实媒体响应先后组合测试。原普通连续录像的逐帧/PCM对照、长暂停和EOF尾部回归另经全量桌面 CTest 执行。测试项增加后只将整个 finite 测试进程上限从150秒增至240秒，各用例的呈现/控制断言期限不变。

## 最终源码与全量回归

最终八份编译输入绑定 `source-final-validation-03.sha256`，清单自身SHA256为 `19b7fca6713c389d8f8288f2753064316f911fff8fa602e3f56bd73283c0aa86`。`tail-green-01` 后仅将新增连续性条件按既有clang-format换行，再重新构建两种配置；EOF的 `>1秒` 条件在该定向GREEN前已存在。最终全量明确绑定清单03，不以较早的清单或全量日志替代。

- Normal：`normal-build-final-02.log` 构建退出0；`normal-full-03.log` 为9/9、373.86秒、退出0，`normal-full-03.raw.log` 中449项QTest结果全部通过（包含各进程初始化/清理），无跳过。包含AAC/PCMA、正常逐帧与PCM完整性、长暂停/EOF、前后跳变、单轨跳变、控制负测、原owner隔离及主窗口交互。
- ASan+UBSan+LSan：`asan-build-final-02.log` 构建退出0；`asan-full-01.log` 为9/9、377.32秒、退出0，`asan-full-01.raw.log` 同为449项通过、0失败/跳过。实际启用 `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`、`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`、`LSAN_OPTIONS=exitcode=23`，未配置抑制或禁用泄漏检测；原始输出未发现ASan/LSan错误或UBSan runtime error。结束后再次核对最终源码及Normal/ASan程序SHA均一致。
- 构建环境：DEV、GCC16.0.1 experimental；Normal为 `-O2 -g -DNDEBUG -Werror`，Sanitizer为 `-O1 -g -DNDEBUG -fsanitize=address,undefined -fno-omit-frame-pointer -Werror`。两种均使用Qt6.2.4和 `/var/tmp/qt-broadcast-sdk-755cf52-z2y4wEha/install` 的同一FFmpeg shared SDK，不是开发机另一份 `/home/gyl/ffmpeg901`。
- `final-binaries.sha256`、`sdk-libraries.sha256`、`fixtures.sha256`、`compiler.txt`、`asan-runtime.txt` 与 `validation_commands.md` 保留实际程序、素材、动态库、编译器及命令。外部Qt/SDL/FFmpeg shared库未完整插桩，不把Qt项目Sanitizer结果扩大为所有依赖的完整内存验证。
- 本轮未修改Go、媒体服务器或third，未重新执行其全套测试；既有Go/真实大华结果仍只属于其原提交和原始证据。未清理原始失败、素材、依赖或其他Agent目录。
- 清理范围遵循用户最终约束：只可处理 `/tmp`、`/var/tmp` 下确认自有且无用的产物。本轮证据仍需保留、构建和SDK仍用于后续开发，没有删除文件，不因目录命名类似便推定归属。

## 本增量结束时尚未完成

- 同一真实大华 dialog，经新 Qt seek 操作、Go INFO、媒体转发到实际呈现的端到端组合；新媒体早/晚于204的集成编排。
- Qt 0.25/0.5/1/2/4倍实际双轨呈现。不能把请求值当实际设备倍率，不用固定48kHz反压成1倍，也不靠静音冒充完成。现有设备PCMA确实随2倍加速的证据仍有效。
- 物理屏幕/声音、不同 NVR、精确定位误差与设备忽略/延迟行为的实机覆盖。

以上是本实现增量结束时的NOT_RUN/未完成状态；后续真实大华限定seek组合见页首链接，其他未覆盖项仍保留。继续推进，不以本增量缩小原七阶段目标。
