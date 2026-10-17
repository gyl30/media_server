# Qt 同会话录像倍率控制入口

## 范围

本增量从 `c3ac9e7` 继续阶段四，先接通播放组件到已有 signaling Scale API 的控制调用与生命周期回归。**不是倍速呈现完成**：没有主窗口倍率按钮，没有将请求倍率写入本地视频或音频时钟。后续仍须在同一真实媒体会话内完成设备实际发送节奏的双轨呈现，不得用本增量代替七阶段目标。

设备控制面已支持基本五种倍率，真实大华双轨输入取证另见[Scale 输入节奏](qt_monitor_client_dahua_scale_input.md)。请求 4× 的实测短窗约 3.43×，204 也不保证设备执行；因此不能把请求值或响应完成时间当作本地时钟倍率或新段边界。

## 调用与所有权

- 新增 `playback_view::scale_record(double)`，仅接受 `0.25/0.5/1/2/4`；精确枚举比较同时拒绝 NaN、无穷、零、负数和未支持的小数。
- 复用 `record_seek_ready()`：原 owner 和 decoder 有效、已有呈现、未暂停、decoder 未结束、没有在途请求。原暂停/继续仍允许 EOF 后的本地尾部控制，不扩大 Scale 门禁。
- 向创建时固定的 `record_owner_` 和 `record_id_` 发送 `POST /api/playbacks/{id}/scale`、`{"scale":数值}`；继续使用原 `request_`、原 `session_` 和 8 秒客户端期限，没有新 HTTP client、状态或计时器。
- pause、seek、scale 共用现有在途请求互斥。切换公共平台不改变原播放 owner；stop 先销毁 QObject context，再取消请求，旧回复不能改变新实例。
- 仅 204 表示协议受理。404 或 `playback_not_streaming` 按原 stop 清理；其他响应、异常 200 或超时显示结果未知，不自动重试或发送相反控制。
- 204 不重开 GET/票据/INVITE、不清空队列、不 flush codec、不修改暂停状态或本地时钟。设备忽略请求时，原 1× 媒体仍应完整呈现。

## 自动化验证

证据位于 DEV `/var/tmp/qt-scale-control-c3ac9e7-MN42BhpD/`。`source-final-02.sha256` 固定三份编译输入；Normal与ASan采用相同源码、Qt6.2.4、GCC16.0.1 experimental和原shared FFmpeg9.0.1 SDK，未新建或替换系统依赖。

- 首轮 `normal-directed-01.log`：18项通过、无失败/跳过、53.485秒；它发生在B资源屏障补强之前，保留但不冒充最终源码回归。
- 补强后 `normal-final-directed-02.log`：18项通过、无失败/跳过、53.541秒；`normal-full-01.log` / `.raw.log`：9/9 CTest、458项QTest结果通过、无失败/跳过、400.66秒，均退出0。
- `asan-directed-01.log`：18项通过、无失败/跳过、53.700秒、退出0；`asan-full-01.log` / `.raw.log`：9/9 CTest、458项QTest结果通过、无失败/跳过、404.43秒、退出0。原始完整日志未见ASan/UBSan/LSan错误；最终源码及程序SHA复核成功。

ASan构建启用`-fsanitize=address,undefined -fno-omit-frame-pointer`及泄漏检测，无抑制或关闭检测；外部Qt、SDL、FFmpeg shared库不是完整插桩版本。两份构建保持原`-Werror`，未调整CTest超时或个别断言门槛；完整命令见证据中的`validate.sh`、`validation_commands.md`。

两次构建均为桌面独立目标；本增量未修改Go或媒体服务器生产实现，未重复Go全套、真实相机或性能容量测试。独立代码/文档审查无P0/P1阻断，原始摘录另存`independent_review.md`。仅清理本轮新建、逐字节核对重复的两个SWR探针副本，共139,940字节；原源码、ELF、日志和SHA保留，清理记录见`own-duplicate-cleanup.log`，没有清理其他目录或其他Agent资产。

`finite_playback_view_test` 新增 `scale_control` 七行：204 但媒体不变、busy、拒绝、到期、非 streaming、异常 200、超时。204 行检查同一 decoder/GET、全部 200 帧实际 paint hash 与本地参考相同、源音频样本和生成 PCM 数量完整、零丢弃、SDL dummy 队列排空与暂停、精确一次 DELETE。生成 PCM 计数和 dummy 排空不等于数字回环或物理听音。该API夹具一次性发送原PS，当前排空耗时下限只证明本增量未按请求直接改时钟，不能将其当作真实1×输入或后续队列自适应的收敛验收；真正忽略Scale的节奏对照须使用下述定时输入。

原固定平台用例扩为 seek 换实例、Scale 停止、Scale 换实例；继续覆盖旧 204 迟到不能写新状态，及原/新 owner 各自清理。原 seek 在途和 EOF 尾部用例追加 Scale 拒绝断言。

## 后续呈现的 RED 与 SDK 实验

仓库外证据：DEV `/var/tmp/qt-scale-control-c3ac9e7-MN42BhpD/`。`finite_playback_view_test.cpp`、`CMakeLists.txt` 是私有 pacing 探针，不是已通过的仓库 CTest；它链接本次 Normal 的 `libdesktop_media.a`。

探针将既有 8 秒 H.264/PCMA 素材四次拼接、连续平移 PES PTS/DTS，按 PS pack 中的 PTS 定时调用原夹具 `socket.write()`；同一 HTTP GET、同一 decoder，不改请求值与204。计划序列是源时间 0–8 秒按1×、8–24秒按2×、其后恢复1×，但本轮在墙钟约15秒主动停止，**未执行恢复段和完整EOF**。paint 观察的是 offscreen Qt 实际 Paint 事件，不是物理显示器刷新。

| 日志 | 源供给 | 观察墙钟窗 ms | 窗内 PS pack | 实际 paint/render | 结果 |
| --- | --- | --- | --- | --- | --- |
| `pacing-red-01.log` | 请求2、定时2× | 11016–15018 | 400 | 100 / 100 | 预期RED，进程退出1 |
| 同日志对照行 | 请求2、定时1× | 11027–15028 | 200 | 100 / 100 | 对照PASS |
| `pacing-red-02.log` | 请求2、定时2× | 11028–15034 | 402 | 100 / 100 | 重复预期RED，进程退出1 |

每400个pack包含200个视频包和200个音频包；2×供给约50视频帧/秒，当前实际呈现仍约25帧/秒。首次呈现/控制就绪约5.14秒，包含现有FFmpeg探轨积压，不是首字节或首pack时刻。两种供给下 decoder 都被有限队列背压，不能将夹具成功接受的写入数冒充解码消费率；定时器允许补发到期数据，亦非逐包实际到达时刻的证明。本结果只证明当前固定1×呈现未跟随该定时2×供给，不证明双轨音频、真实设备或精确倍率。完整字节SHA、程序/SDK身份和原始退出码另存同目录。

独立 `swr_rate_probe.cpp` 用当前shared SDK验证后续音频变速的基础API：48kHz stereo S16、每次960帧、两秒共96000个每声道输入样本，强制重采样并每次续设补偿 `delta=round(48000*(1-rate))`、`distance=48000`，随后再查询输出容量。`swr-counts-01.log` 中 `.25/.5/1/2/3.43/4` 六档分别输出384000/192000/96000/48000/27989/24000个每声道样本；3.43档非整数取样按实际舍入，不伪造精确小数样本。各档均真实flush至返回0，进程退出0，探针启用ASan/UBSan/LSan且无报告。

重要尾部事实：flush输出已完整、再调用返回0后，`swr_get_delay()`仍有16/17个输入样本，3.43档为14。因此后续不能用 `delay==0` 判断排空，也不能直接以 `input_end-delay` 作为真实末端。这个实验不覆盖动态倍率切换、Qt/SDL输出时间映射、定位、实际声音或外部shared库完整插桩，不能替代播放器变速验收。

另存的 `swr_tone_probe.cpp` / `swr-tone-01.log` 保留相同计数，补验实际输出非零：六档单声道 RMS 为7070.935–7071.231，各有2000次正向零交叉、两个声道样本一致，退出0。只证明该2秒数字1kHz输入在固定六档转换时输出非静音、总正向零交叉数符合2000周期，不是逐周期连续性、动态切换、抗混叠全频带或播放设备验收；这种重采样会随速度变调，不是保音高的atempo。计数版原源码、ELF和SHA均保留且重新核对。

## 尚未完成

- 主窗口倍率操作，以及 Qt 按实际设备输入节奏呈现音视频。
- 定时 PS 输入 1→2→1、慢倍速、请求被忽略、实际倍率不同于请求的完整回归；不能仅按解码计数或连接成功判断。
- 变速音频实际输出样本/尾部、暂停恢复、跨轨时间戳跳变、EOF、关闭及有界队列验证。
- 当前 Qt 操作到真实大华的 Scale 双轨呈现和实际数字音频回环。

普通连续录像的逐帧、PCM、暂停和定位完整性门槛保持不变。不能为加速而静音、丢帧、删除原 PS 或扩大无界缓冲；也不承诺物理显示器能显示 4×25fps 的每一帧。
