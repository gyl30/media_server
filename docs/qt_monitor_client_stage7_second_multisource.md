# 第二机16个独立源实时解码补验

2026-10-18，clean `d06f0c0891a95b0d99c21b0fb3f17a898535ccd1`。开发机只发布和控制，实际Qt、CPU解码、PCM及16格呈现在TEST `172.20.63.32`。本次是短时跨机功能验收，不是容量或长期性能基线。

## 条件与可追溯性

- DEV `172.20.45.187`，独立监听26980–26984，两个media worker、独立SQLite及控制凭据；不改现有服务或系统参数。
- TEST Ubuntu22.04/glibc2.35，Xeon Silver4216、32逻辑CPU；Qt6.2.4、FFmpeg9.0.1 shared SDK、CPU解码。offscreen/SDL dummy，未操作用户物理桌面。
- 16个不同push设备、不同stream/token、各自有限90秒FFmpeg发布者；8路RTMP H.264/AAC、4路RTSP/TCP H.264/PCMA8k单声道、4路RTMP无音轨。视频1920×1080、25fps、约4Mbps。内容来自既有相同编码fixture，不冒称16台不同摄像机或16种画面。
- Qt探针链接生产monitor_window与播放组件，模拟窗口操作并各自申请独立观看，真实DEV→TEST RTSP/TCP。不是对已安装gb_monitor ELF的外部操作验收，也不是WHEP/browser/UDP吞吐验证。
- 正常增量构建与当前Go编译成功，源码前后SHA一致；C++ ELF未因只改打包脚本而变化，不宣称fresh C++编译。未重跑本轮未涉及的完整CTest、Go或Sanitizer。

| 构件 | SHA256 |
| --- | --- |
| 归档 | `91fc9fde58a51480df9305754094ade85c49f5ba27156ad73cbac99ca75c0355` |
| manifest（git_dirty=false） | `250917ea868a1238fc49cee7f4ffc3ba698c21dbb4f8b25083e2ab91bd2ff505` |
| 构建树 gb_monitor（安装前） | `54e1aac16d9697ddcef13d058425ec6ce3a2b7a4588659b749ffa54bb4ee57b1` |
| 包内及TEST安装版 gb_monitor | `8dcf4a71e613f8dcee9aae3dbc1875c0db0b7026b283a647bf2f6ba3461273d8` |
| supplemental playback probe | `fe3d4dd5340146c4480103a5f9aa238e7b75f23c26e337a8040950f4dd9978c1` |
| media_server | `45f0c48affb7b866578178f5298fe1291d6c6efbeab7f79c34366f54dda4fde4` |
| signaling | `592cc54945cd2ad5393284b8e21db30e517a6933d48e839fa66ecfbac31c8501` |

probe是单列工具，不篡改包manifest冒充产品安装程序；当前构建日志和源码SHA将它关联到此HEAD。TEST逐文件SHA、ldd-r及真实Qt maps核对通过，包括[动态libproxy闭包修复](qt_monitor_client_runtime_proxy.md)。63次maps采样未见包外非glibc DSO，不扩大为排除了任何采样间隔内的瞬态dlopen。公开发行许可仍BLOCKED。

2026-10-18独立只读复核补正：上表原来未区分构建树与安装ELF。`desktop/package_internal_runtime.py:175`执行`cmake --install`，`desktop/CMakeLists.txt:24`的`$ORIGIN/../lib`使生成安装脚本执行RPATH_CHANGE，因此同为12473616字节的ELF具有不同SHA。manifest的`build_binary_sha256`记录54e1…，`files["bin/gb_monitor"]`记录8dcf…，不能互换。已在TEST原包`/var/tmp/qt-second16-runtime-d06f0c0-2xHxobof/runtime/bin/gb_monitor`及同目录原tar成员实际复算8dcf…；原归档及manifest的SHA仍与上表一致。未替换原包、修改manifest或重跑媒体。

## 预设门槛与实际结果

沿用原16源验收：全部画面及各预期音轨ready后开始共同30秒窗口，每格至少95%目标25fps解码/呈现、逐秒视频及预期音频PTS/PCM进展、零错误和零丢弃；停15格后余下1格继续至少3秒，最后有界停止、析构及资源归零。没有因前两轮失败改变媒体、阈值、窗口或清理断言。

本轮监督器2026-10-17T23:16:29Z至23:18:06Z，97.226秒退出0，未触发安全停止。实际Qt进程2243837、starttime3236632833；36.474秒内正常退出0，无强制终止，**1950项断言零失败**。

共同窗口以 `observation_start` 到最后一条 `observing` 为准：**30.002秒**，而不是已停止15格后的 `before_stop`。16格各解码749–750帧，真实Qt paint748–750帧，全部H.264；8AAC/4PCMA预期音轨均有持续PCM，4无音轨保持合法无音频。各格解码错误、视频/音频丢弃均零。

| 轨道 | 30.002秒共同窗口PCM帧增量（每声道时间单位） |
| --- | --- |
| 8路AAC | 每路1,438,720–1,440,768，输出48kHz |
| 4路PCMA | 每路1,436,772–1,439,844，经既有SWR输出48kHz |
| 4路无音轨 | 0；音频PTS为null，不补造零音频时长 |

首帧2862–3162ms。停15格后15个decoder析构，余格3秒内继续75视频解码/paint帧、144384 PCM帧；最后总停止等待99ms、析构附加等待0ms，未触发12秒/2秒期限。

TEST实际Qt进程63条资源采样，采样最大PSS240322KiB、最大FD48；不是精确峰值或稳定容量指标。SDL dummy仅说明本地产生PCM/输出调用与唯一声音选择，**不代表设备扬声器、物理听音或屏幕视觉验收**。

16个发布源各在启动后90.002–90.033秒自然退出0，禁止以TERM/KILL替代EOF；全部19个自有进程（含SSH、服务）退出0、PID/进程组消失。16个配置已删，receiver最终0，TCP26980–26984与SIP UDP26984可重新绑定。TEST Qt PID/组消失，正常释放。

## 原始失败与边界

旧 `4f4cd12` 包的run-01因私有脚本使用wildcard地址被配置拒绝；run-02则在Qt启动约0.566秒时被未打包libproxy模块的原映射guard阻断。后者是真实包依赖缺口，已最小修复，不以放宽guard通过。两轮原FAIL及清理证据原样保留，不能改记为媒体PASS。新轮后处理脚本首次未转换JSON字符串计数、又误取stop后的before_stop，已另存修正为最终observing窗口；这些离线摘要不参与原Qt1950项断言或最终退出判定，未重跑媒体隐藏失败。

DEV原预检global I/O PSI很高：只读调查发现其他GUI的io_uring等待与该scope PSI一致，块盘繁忙和实际读写增量不对应；未停止或修改该进程。仅本轮scope另行采样并设内存/磁盘/IO及总期限停止条件，本轮未触发。不由此推导整机无任何I/O压力，更不把本次作为性能测量。

本次补齐第二机受控16独立源数字解码/呈现与清理，**不是整个平台GO**：真实相机公共对讲与回传/物理声学、全部设备控制和公开分发边界仍按原计划单列。未新增生产状态或改变授权、源代际、播放队列与资源所有权；不得删去仍有价值的原失败/生命周期覆盖。

## 原始证据

- DEV：`/var/tmp/qt-second16-d06f0c0-x0DEiJPz/`，正式结果 `run-01/result.json`、`watch-result.json`、构建/manifest/SHA、原始Qt证据及脚本；`observation-summary.json`仅是正确窗口的派生摘要。
- TEST包：`/var/tmp/qt-second16-runtime-d06f0c0-2xHxobof/`，原包和归档。
- TEST运行：`/var/tmp/qt-second16-run-d06f0c0-x0DEiJPz/`，原始Qt JSON/PNG、进程身份、maps、资源采样、stdout和清理结果。
- 原失败DEV：`/var/tmp/qt-second16-4f4cd12-TngcgwrF/run-01/`、`run-02/`；TEST run-02：`/var/tmp/qt-second16-run-4f4cd12-TngcgwrF-run02/`。

Qt原始JSON SHA `fdc7165b3310a9f90dbf3e6ea7ccc4b11c98eb0321304967405c539cdbc07f27`；maps SHA `0a9ab0ab9eea4c8430936e597bda00a4c7832a93a590a6e63b2a87e1ae8ba7d0`；资源采样 SHA `255a76bc237af1bc91f1913327f99e0d51deab956bc60e78b2eb5cf9d3a4023c`。DEV收到的12个顶层普通文件逐项与TEST SHA一致；不复制XDG socket目录、不删除原始副本。日志/URL可能含凭据，原始材料不提交Git。
