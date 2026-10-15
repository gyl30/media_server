# Qt 监控客户端

Linux x86_64 原生 Qt Widgets 客户端，沿用传统桌面监控原型。当前为阶段一单路观看实现，不是完整 GB28181 平台功能验收。

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

## 观看与生命周期

- 视频：H.264；H.265 尚未实现。
- 音频：AAC、PCMA、PCMU，转为 48kHz/stereo/S16 输出；合法无音频源正常观看。已声明而不支持的音轨明确失败，不静默丢弃。
- 默认静音，但音轨仍真实解码。首次开声才创建设备，同一预览静音时暂停并清空队列，重新开声复用设备；停止、源结束或设备错误才关闭设备。设备失败会转静音并提示，不自动重开。
- 画面只等待自然关键帧，不请求反向关键帧，不恢复 GOP/history。
- GB 先点播，再在总计 15 秒内查询同一个 live 的 `media_ready`；沿用原票据，不重复点播或重签。该字段源自服务器约 5 秒一次的对账缓存，不是瞬时媒体反馈。旧信令未返回字段时明确提示升级。
- 普通源沿用现有媒体就绪校验。开流及轨道探测合计 15 秒；实际视频/音频解码各有独立 10 秒进展期限。GOP 过长可能超时；失败只允许用户手动重试。
- 停止/替换销毁本次请求 context，并中断独立解码线程。GUI 不等待网络 join；旧回调不能修改新画面。关闭 viewer 不停止共享 GB 上游。
- SDL 在进程入口初始化和退出；画面只持有自己的设备。队列有界，音频时钟包含双 period 的估算，不宣称精密硬件同步。

## 真实播放探针

`desktop_playback_probe` 是短时功能验收工具，不是容量发生器。选择一个已存在的源，每个画面独立签票：

```sh
QT_QPA_PLATFORM=offscreen ./build-desktop/desktop_playback_probe \
  --platform http://127.0.0.1:9090 --device 34020000001320000001 \
  --channel 34020000001310000001 --views 1 --seconds 10 --output /tmp/qt-play-new.json
```

`--push-device ID`、`--source-id ID` 与 `--device ID --channel ID` 三选一；不是提交发布 token。`--views` 为 1–16，`--audible` 只让第一路出声；`--toggle-audio` 需配合 `--audible`，每两秒主动切换声音以检查播放连续性及退出清理。输出文件必须不存在，权限设为 0600；不写播放 URL/token。

探针先等每路视频交给 view、已声明音轨实际解码，再开始共同观察；逐秒检查双轨 PTS/PCM/视频进展，并检查窗口内至少 95% 标称视频率、PCM 采样率及交给 view 的视频比例。`rendered_frames` 是交给 QWidget 的图像数，不是显示器扫描/垂直同步计数。PCM、audible 标志也不能替代声音设备或声学验收。退出后独立观察线程结束和 QObject 析构。

探针提供多路能力不表示主窗口已完成多画面交互。阶段二才验收 1/4/9/16、选中画面及真实 16 路实时解码。

## 发布边界

正常服务器用的 `/home/gyl/ffmpeg901` 是启用 GPL/version3 的静态 FFmpeg，不能不加区分地当作桌面发行依赖。本轮另行构建同版本、不启 GPL/version3/nonfree 的 shared decoder SDK，原 SDK 不改。动态依赖、Qt 插件、实际构建参数、许可材料和宿主 ABI 必须随内部运行包核对；仓库项目许可证尚未确定，内部验证通过不代表对外二进制分发许可已经闭合。

原型位于开发机 `designs/gb28181-desktop-client/`，按要求不提交。后续 PTZ、录像、下载、报警和对讲入口尚未实现，不以禁用按钮冒充功能。

内部搬运目录可在已完成 shared SDK 构建后生成（输出路径必须不存在）：

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
