# Qt 监控客户端

Linux x86_64 原生 Qt Widgets 客户端，沿用传统桌面监控原型。当前支持1/4/9/16画面观看，不是完整 GB28181 平台功能验收；实测硬件、媒体及边界见 [阶段二验证](../docs/qt_monitor_client_stage2.md)。

控制面使用 signaling HTTP API；每个画面独立申请一次性播放票据，媒体使用 FFmpeg 的 RTSP/TCP 输入。不嵌入网页，不维护另一套 VLC 播放器，也不新增私有传输协议。

## 构建

需要 CMake ≥3.22、C++20 编译器、Qt ≥6.2 的 Widgets/Network（测试还需 Test）、SDL2 ≥2.0.20，以及 FFmpeg ≥6.0 的 avformat/avcodec/avutil/swscale/swresample shared 开发库。桌面使用动态库依赖，不从静态 SDK 的 private 库清单拉入无关组件。实际验证的编译器是 GCC 16.0.1 experimental，Qt 6.2.4，SDL2 2.0.20，FFmpeg 9.0.1。

```sh
PKG_CONFIG_PATH="$FFMPEG_PREFIX/lib/pkgconfig" cmake -S desktop -B build-desktop \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON
cmake --build build-desktop -j4
FFMPEG_TEST_EXECUTABLE=/path/to/ffmpeg ctest --test-dir build-desktop --output-on-failure
./build-desktop/gb_monitor
```

`FFMPEG_PREFIX` 是本地已安装的 FFmpeg SDK 路径，不是固定用户目录。构建只针对独立 `desktop/`，不改变媒体服务器的 CMake 依赖配置。fixture 生成 CLI 必须包含 libx264/AAC/G.711/Opus 编码能力；播放库无需包含这些编码器。缺少 CLI 时解码测试会跳过，必须检查 QTest 的 skipped 计数，不能把它计为解码通过。

在客户端平台地址栏填 signaling 地址，例如 `http://127.0.0.1:9090`，刷新设备后双击通道/源预览。支持网关路径前缀；不接受 URL 内嵌凭据、query 或 fragment。API 认证沿用部署网关，不另造客户端密码协议。

先点击目标画面，再双击设备通道；也可把树中的通道/普通源拖到目标画面。底部或画面右键切换1/4/9/16布局，缩布局会停止隐藏的会话。声音按钮只作用于当前选中画面；选择本身不切换声音。停止/替换默认静音，需重新显式开声。支持全屏/Escape、保持比例及PNG抓图（当前显示分辨率）。偏好只保存平台地址、布局、比例和窗口位置，不保存票据或恢复自动播放。

选中国标设备或通道后可“查询目录”；202及界面“已受理”仅表示SIP MESSAGE被接受，稍后手动双击设备查看实际返回的通道，不表示目录已收齐。

右侧“基本”页可显式查询设备信息、设备状态；选中通道时查询其所属注册设备，不宣称通道级状态。信息只显示本次实际响应，不缓存或自动轮询；设备自报在线状态不替代平台注册在线状态。切换设备、刷新、换平台或关闭会隔离旧回复。`Result=ERROR` 即使 HTTP 200 也明确显示设备拒绝；超时只允许手动重试。该功能要求同版本 signaling 的 `POST /api/devices/{id}/query`，旧平台返回404时不走兼容旁路；验证与边界见 [查询子集验证](../docs/qt_monitor_client_stage3_queries.md)。

选中国标通道后，“预置位”页可查询该通道实际返回的编号和名称；空列表是有效结果。编号保留设备返回的字符串，不按后续控制指令的数值范围截断。查询与基本页请求串行，不缓存、不自动刷新；切换目标立即清空旧列表。该功能要求同版本 signaling 的 `POST /api/devices/{id}/channels/{channel_id}/presets/query`；设置、调用、删除尚未实现。详见 [预置位查询验证](../docs/qt_monitor_client_stage3_presets.md)。

“云台”页对当前树中明确选中的国标通道提供八方向点动、变倍与 STOP；目标区同时显示父 ID 与通道 ID，不跟随播放画面的选中状态。方向速度 1–255（默认64），变倍1–15（默认4），按钮不自动重复。平台以300毫秒为点动等待上限，随后独立尝试 STOP；手动 STOP 可提前结束等待，重复 STOP 不取消正在发送的 STOP。SIP 200 只是受理，界面始终显示物理结果未确认。当前通道的 STOP 在普通查询或点动等待期间仍可用；它替换当前 UI 等待，原动作最终 STOP 的结果此后只在平台日志可查。切树、刷新或关窗只隔离 UI 回调，不撤销已发 HTTP/SIP。该功能要求同版本 `POST /api/devices/{id}/channels/{channel_id}/ptz`；网络中断或进程崩溃不能保证物理停止，真实设备动作尚待验收。详见 [点动与停止验证](../docs/qt_monitor_client_stage3_ptz.md)。

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

原型位于开发机 `designs/gb28181-desktop-client/`，按要求不提交。聚焦/光圈/巡航、预置位写操作、录像、下载、报警和对讲尚未实现，不以禁用按钮冒充功能。

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
