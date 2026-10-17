# Qt 监控客户端

Linux x86_64 原生 Qt Widgets 客户端，沿用传统桌面监控原型。当前支持1/4/9/16画面观看，不是完整 GB28181 平台功能验收；实测硬件、媒体及边界见 [阶段二验证](../docs/qt_monitor_client_stage2.md)。

控制面使用 signaling HTTP API；每个画面独立申请一次性播放票据，实时媒体使用 FFmpeg 的 RTSP/TCP 输入。独立历史回放采用平台有限 TCP/RTP-PS 通道，经 HTTP chunked 输入同一 FFmpeg 解码器。不嵌入网页，不维护另一套 VLC 播放器。

## 构建

需要 CMake ≥3.22、C++20 编译器、Qt ≥6.2 的 Widgets/Network/WebSockets（测试还需 Test）、SDL2 ≥2.0.20、Boost ≥1.89 头文件，以及 FFmpeg ≥6.0 的 avformat/avcodec/avutil/swscale/swresample shared 开发库。Boost.Beast 只用于有限 HTTP 分帧解析，不链接 Boost.Context。桌面使用动态库依赖，不从静态 SDK 的 private 库清单拉入无关组件。实际验证的编译器是 GCC 16.0.1 experimental，Qt 6.2.4，SDL2 2.0.20，Boost 1.92.0，FFmpeg 9.0.1；有限录像还要求 SDK 启用 MPEG-PS demuxer；本地录制与设备下载要求启用 Matroska muxer 与 extract_extradata bsf（aac_adtstoasc 随 muxer 自动选中）；麦克风上传要求 pcm_alaw encoder。Qt WebSockets 必须与实际 Qt 库匹配，不自行实现 WebSocket 协议。

```sh
PKG_CONFIG_PATH="$FFMPEG_PREFIX/lib/pkgconfig" cmake -S desktop -B build-desktop \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON
cmake --build build-desktop -j4
FFMPEG_TEST_EXECUTABLE=/path/to/ffmpeg ctest --test-dir build-desktop --output-on-failure
./build-desktop/gb_monitor
```

`FFMPEG_PREFIX` 是本地已安装的 FFmpeg SDK 路径，不是固定用户目录。构建只针对独立 `desktop/`，不改变媒体服务器的 CMake 依赖配置。fixture 生成 CLI 必须包含 libx264/AAC/G.711/Opus 编码能力；播放库不需要视频编码器，对讲需要上述 PCMA 编码器。缺少 CLI 时解码测试会跳过，必须检查 QTest 的 skipped 计数，不能把它计为解码通过。

在客户端平台地址栏填 signaling 地址，例如 `http://127.0.0.1:9090`，刷新设备后双击通道/源预览。支持网关路径前缀；不接受 URL 内嵌凭据、query 或 fragment。API 认证沿用部署网关，不另造客户端密码协议。

先点击目标画面，再双击设备通道；也可把树中的通道/普通源拖到目标画面。底部或画面右键切换1/4/9/16布局，缩布局会停止隐藏的会话。声音按钮只作用于当前选中画面；选择本身不切换声音。停止/替换默认静音，需重新显式开声。支持全屏/Escape、保持比例及PNG抓图（当前显示分辨率）。偏好只保存平台地址、布局、比例和窗口位置，不保存票据或恢复自动播放。

选中国标设备或通道后可“查询目录”；202及界面“已受理”仅表示SIP MESSAGE被接受，稍后手动双击设备查看实际返回的通道，不表示目录已收齐。

“开始对讲”上送本机默认麦克风，目标是当前选择的父国标设备；设备回传声音仍由对应实时画面的“开启声音”独立控制。先准备采集和 PCMA 编码，再申请一次性上传 URL，经真实 WebSocket 连接后才开始采集。停止、切换平台及关闭窗口会关闭采集并精确删除本次 talk_id；建立中的取消仍等待有界响应清理，不误删其他 owner。当前只验证直连 LAN 的音频上传，状态“上送麦克风”不代表设备听音或完整双向对讲验收。前后端必须同版本，旧平台不走兼容旁路。详见[Qt 音频上传增量](../docs/qt_monitor_client_stage6_qt_talk.md)。

右侧“基本”页可显式查询设备信息、设备状态；选中通道时查询其所属注册设备，不宣称通道级状态。信息只显示本次实际响应，不缓存或自动轮询；设备自报在线状态不替代平台注册在线状态。切换设备、刷新、换平台或关闭会隔离旧回复。`Result=ERROR` 即使 HTTP 200 也明确显示设备拒绝；超时只允许手动重试。该功能要求同版本 signaling 的 `POST /api/devices/{id}/query`，旧平台返回404时不走兼容旁路；验证与边界见 [查询子集验证](../docs/qt_monitor_client_stage3_queries.md)。

选中国标通道后，“预置位”页可查询该通道实际返回的编号和名称；空列表是有效结果。编号保留设备返回的字符串，不按控制指令的数值范围截断。查询与基本页请求串行，不缓存、不自动刷新；切换目标立即清空旧列表。该功能要求同版本 signaling 的 `POST /api/devices/{id}/channels/{channel_id}/presets/query`。详见 [预置位查询验证](../docs/qt_monitor_client_stage3_presets.md)。

预置位“设置”使用数字输入框1–255；“调用”和“删除”使用查询列表中当前明确选中的规范十进制编号1–255。`001`、`camera-A`、`1000`等原样显示，但不隐式转换、夹值或截断后控制。设置和删除须在非模态确认框核对父设备、通道及编号；默认取消，确认前不发指令，切树、刷新、STOP或退出会关闭旧确认。确认后发送沿用 `/ptz` 的 `preset_set/preset_goto/preset_remove` 与整数 `preset_id`，不支持名称写入。设置/删除发送后清空旧列表，请手动查询确认，不自动刷新或乐观改列表。调用受理后不自动300毫秒STOP，也不表示已到位；需要时显式STOP。SIP事务取消/失败仍独立尝试STOP，但不回滚可能已经生效的设置/删除。已受理调用不再由平台在途owner跟踪，切目标、关窗或平台退出不保证停止设备；后续点动/STOP可能中断它，全零STOP能否中断调用须实测。物理保存、到位、删除和停止仍待验收。见 [预置位控制验证](../docs/qt_monitor_client_stage3_preset_control.md)。

“云台”页对当前树中明确选中的国标通道提供八方向点动、变倍与 STOP；目标区同时显示父 ID 与通道 ID，不跟随播放画面的选中状态。方向速度 1–255（默认64），变倍1–15（默认4），按钮不自动重复。平台以300毫秒为点动等待上限，随后独立尝试 STOP；手动 STOP 可提前结束等待，重复 STOP 不取消正在发送的 STOP。SIP 200 只是受理，界面始终显示物理结果未确认。当前通道的 STOP 在普通查询或点动等待期间仍可用；它替换当前 UI 等待，原动作最终 STOP 的结果此后只在平台日志可查。切树、刷新或关窗只隔离 UI 回调，不撤销已发 HTTP/SIP。该功能要求同版本 `POST /api/devices/{id}/channels/{channel_id}/ptz`；网络中断或进程崩溃不能保证物理停止，真实设备动作尚待验收。详见 [点动与停止验证](../docs/qt_monitor_client_stage3_ptz.md)。

选中国标通道后可打开独立“录像查询”窗口，手动填写设备当地墙上时间和类型。开窗不发请求，窗口固定父/通道，切换树不暗改目标；刷新、换平台、关闭及重新打开会取消旧查询。查询等待平台15秒，客户端20秒，每次最多1000条，超限提示缩小范围。显示设备原样目录，不按路径或内容去重；“设备声明N条、已收到N条”不代表唯一库存或文件可回放。取消不停止实时观看、共享上游或PTZ。该功能要求同版本 `POST /api/devices/{id}/channels/{channel_id}/records/query`。详见 [录像查询子集](../docs/qt_monitor_client_stage4_record_query.md)。

明确选择目录项并核对设备 UTC 偏移后，可“在选中画面回放”。时间按固定偏移换算，不自动推断历史夏令时；文件路径不作为 URL。客户端创建独占历史实例并一次认领有限 PS 输入；关闭、替换、失败及自然呈现结束只 DELETE 本次实例，不停止其他实时上游。有限队列背压、不主动丢帧；正常结束表示传输和解码呈现结束，不证明请求范围库存完整。底部“暂停回放/继续回放”对选中画面发送同会话 INFO（GB/T 28181—2016 附录 B），平台受理后才冻结画面与声音；暂停占用本次回放期限，继续时实例已结束则停止本次回放。seek 与回放倍速尚未实现，物理 NVR 验收未完成。详见 [有限呈现增量](../docs/qt_monitor_client_stage4_finite_view.md)。

录像窗口可把选中目录项“下载选中录像”到保存目录（默认系统影片目录，与本地录制共用），倍速 1/2/4 由平台以 `s=Download`、`a=downloadspeed` 向设备请求；设备是否支持以实际应答为准。客户端只流复制 H.264 与 AAC/PCMA/PCMU 到 `<通道>_<开始>_<结束>.mkv`，不转码；只有 HTTP chunked 完整结束且解复用正常 EOF 才保存，取消、关闭窗口、损坏包、探测后出现新音轨或写盘失败都删除本次临时 `.part` 并只回收本次实例。完成也不代表请求范围库存完整。

底部“开始录制/停止录制”对选中的实时画面另签一张独立票据，流复制为 `<名称>_<时间>.mkv`，从首个可解码关键帧写起；用户停止、画面停止或替换都写完整封装保存，源结束或网络中断也保存并提示中断，写盘失败删除临时文件。画面右键可设置录像目录。退出程序会等待录制收尾或下载清理完成。详见 [阶段五录制与下载](../docs/qt_monitor_client_stage5_record_download.md)。

## 观看与生命周期

- 视频：H.264；H.265 尚未实现。
- 音频：AAC、PCMA、PCMU，转为 48kHz/stereo/S16 输出；合法无音频源正常观看。已声明而不支持的音轨明确失败，不静默丢弃。
- 默认静音，但音轨仍真实解码。窗口首次开声才创建唯一设备，画面临时借用；静音、停止、替换或源结束时暂停并清空队列、归还借用，下次开声复用。只有窗口销毁或设备错误才Close；设备失败转静音并提示，不无限自动重开。
- 画面只等待自然关键帧，不请求反向关键帧，不恢复 GOP/history。
- GB 先点播，再在总计 15 秒内查询同一个 live 的 `media_ready`；沿用原票据，不重复点播或重签。该字段源自服务器约 5 秒一次的对账缓存，不是瞬时媒体反馈。旧信令未返回字段时明确提示升级。
- 普通源沿用现有媒体就绪校验。开流及轨道探测合计 15 秒；实际视频/音频解码各有独立 10 秒进展期限。GOP 过长可能超时；失败只允许用户手动重试。
- 停止/替换销毁本次请求 context，并中断独立解码线程。GUI 不等待网络 join；旧回调不能修改新画面。关闭 viewer 不停止共享 GB 上游。
- SDL 在进程入口初始化和退出；窗口拥有唯一设备，画面只保存当前借用。旧画面的停止/迟到finished不能影响新借用者。队列有界，音频时钟包含双 period 的估算，不宣称精密硬件同步。

## 真实播放探针

`desktop_playback_probe` 是短时功能验收工具，不是容量发生器。选择一个已存在的源，每个画面独立签票：

```sh
QT_QPA_PLATFORM=offscreen ./build-desktop/desktop_playback_probe \
  --platform http://127.0.0.1:9090 --device 34020000001320000001 \
  --channel 34020000001310000001 --views 1 --seconds 10 --output /tmp/qt-play-new.json
```

`--push-device ID`、`--source-id ID` 与 `--device ID --channel ID` 三选一；不是提交发布 token。`--views` 为 1–16，`--audible` 只让第一路出声；`--toggle-audio` 需配合 `--audible`，每两秒主动切换声音以检查播放连续性及退出清理。输出文件必须不存在，权限设为 0600；不写播放 URL/token。

探针先等每路视频交给 view、已声明音轨实际解码，再开始共同观察；逐秒检查双轨 PTS/PCM/视频进展，并检查窗口内至少 95% 标称视频率、PCM 采样率及交给 view 的视频比例。`rendered_frames` 是交给 QWidget 的图像数，不是显示器扫描/垂直同步计数。PCM、audible 标志也不能替代声音设备或声学验收。退出后独立观察线程结束和 QObject 析构。

加 `--monitor-window` 使用真正监控窗口和设备树输入信号，画面数仅可为1/4/9/16，不等同独立发行ELF的外部鼠标验收。加 `--stop-others-after-observation`（需此模式且至少4画面）在原共同窗口全部通过后停止除最后一个外的画面，再检查3秒幸存画面双轨进展及已停止解码线程析构；不会把有意停止混入原观测窗口。

## 发布边界

正常服务器用的 `/home/gyl/ffmpeg901` 是启用 GPL/version3 的静态 FFmpeg，不能不加区分地当作桌面发行依赖。本轮另行构建同版本、不启 GPL/version3/nonfree 的 shared decoder SDK，原 SDK 不改。动态依赖、Qt 插件、实际构建参数、许可材料和宿主 ABI 必须随内部运行包核对；仓库项目许可证尚未确定，内部验证通过不代表对外二进制分发许可已经闭合。

报警页显示已注册设备及所属通道上报的近期报警，激活时每 2 秒读取一次，最多显示 500 条。平台只在内存保留最近 1000 条；重启会重置列表，容量淘汰会提示缺口，不是持久化或无损报警档案。查询失败保留已有列表，切换平台会取消旧请求。当前没有报警订阅或复位；对讲上传与设备听音的边界见前述音频上传章节。接口、游标和验收边界见 [阶段六报警](../docs/qt_monitor_client_stage6_alarms.md)。

原型位于开发机 `designs/gb28181-desktop-client/`，按要求不提交。聚焦/光圈/巡航、回放 seek/倍速和完整双向对讲尚未验收，不以禁用按钮或仅上传成功冒充功能；不提供独立广播产品入口。

内部搬运目录可在已完成 shared SDK 构建后生成（输出路径必须不存在，其父目录须已存在）：

```sh
python3 desktop/package_internal_runtime.py \
  --build-dir "$PWD/build-desktop" \
  --ffmpeg-prefix "$FFMPEG_PREFIX" --ffmpeg-source "$FFMPEG_SOURCE" \
  --qt-plugin-dir /usr/lib/x86_64-linux-gnu/qt6/plugins \
  --output-dir /tmp/gb-monitor-internal-new
cd /tmp/gb-monitor-internal-new
sha256sum -c SHA256SUMS
./run.sh
```

Qt 插件路径必须与实际链接的 Qt 匹配，示例路径属于已验收 Ubuntu 22.04。包内 `manifest.json` 记录 HEAD、dirty、源码及依赖 SHA、库来源、构建和许可边界。此工具只验证内部动态依赖闭包，不签发公开发行授权；不自动复制 glibc、加载器、字体、显卡驱动、音频服务。ldd 不能穷尽运行时 dlopen：GL vendor、ALSA 插件、NSS/gconv 等仍由同代目标系统提供，不能宣称通用 Linux 兼容。私有进程环境由 `run.sh` 设置，不修改系统配置。
