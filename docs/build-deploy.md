# 构建与部署

## 媒体服务

根 [`CMakeLists.txt`](../CMakeLists.txt) 要求 CMake ≥3.21、C11/C++23 编译器、pkg-config、make、perl 与 nasm。Boost（context/program_options 及 Asio/Beast 等头文件，≥1.89）、OpenSSL 和 jemalloc（普通构建）由 [`deps/build.sh`](../deps/build.sh) 按 [`deps/sources.txt`](../deps/sources.txt) 下载固定版本源码、校验 sha256 后编译为静态库，安装到项目内的 `deps/install`（源码包缓存在 `deps/cache`，均不纳入版本控制），并写入 `BUILD-INFO`（`sources.txt` 与 `build.sh` 的摘要、C/C++ 编译器版本）；用 `CC`/`CXX` 指定与服务端相同的编译器。校验失败、中断或出错时脚本删除半成品；已有完整 `deps/install` 时拒绝覆盖，重新构建前先删除它。OpenSSL 使用 3.5 LTS（支持到 2030-04），libsrtp 的 CI 也只覆盖 3.x。其余依赖都在 `third/`：ada、libsrtp（使用 OpenSSL）、spdlog（仅头文件，使用自带 fmt）、simdjson（关闭异常接口）直接参与构建，ireader 源码在仓库内；FFmpeg、opus、fdk-aac 由 [`cmake/ffmpeg.cmake`](../cmake/ffmpeg.cmake) 以 ExternalProject 在构建目录内编译为静态库（安装到 `<构建目录>/third-install`），只启用服务端用到的组件。

在新 checkout 中先按 `.gitmodules` 初始化依赖（FFmpeg 等使用浅克隆）；已有工作区保持第三方目录及其本地状态。CMake 只从 `deps/install` 查找这些依赖：`BUILD-INFO` 缺失、与当前 `sources.txt`/`build.sh`/编译器不符，或缓存中的 OpenSSL、Boost、jemalloc 路径不在 `deps/install` 下（例如旧构建目录缓存了系统库）时配置直接失败，按提示重建依赖或删除该构建目录的 `CMakeCache.txt`。`sources.txt`、`build.sh` 或 `BUILD-INFO` 变化会触发重新配置；opus、fdk-aac、FFmpeg 子模块 HEAD 变化会触发 FFmpeg 重新构建。每个构建目录首次构建会先编译 FFmpeg 等依赖。

GCC 构建静态链接 libstdc++、libgcc 和 sanitizer 运行时，产物动态依赖只有 libc、libm 和 libgcc_s（CMake 为 C 隐式库追加），不受 `ld.so` 缓存里其他 GCC 版本运行库的影响。`media_server --version` 和启动日志首行输出源码版本（`git describe --dirty`）、编译器、Boost、OpenSSL、FFmpeg、opus、libsrtp 和分配器版本；ELF 带 build-id。

```sh
git submodule update --init --recursive
CC=gcc-16 CXX=g++-16 deps/build.sh
cmake -S . -B build-prod -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=gcc-16 -DCMAKE_CXX_COMPILER=g++-16
cmake --build build-prod --target media_server -j4
./build-prod/media_server --version

# 性能客户端采用独立构建目录。
cmake -S . -B build-perf -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=gcc-16 -DCMAKE_CXX_COMPILER=g++-16 -DMEDIA_SERVER_BUILD_BENCHMARKS=ON
cmake --build build-perf -j4
```

`MEDIA_SERVER_HARDENING` 默认 ON；`MEDIA_SERVER_SANITIZER` 接受 `none/address/undefined/thread`。address 与 thread 模式要求 ucontext 实现、带对应 sanitizer 注解的 Boost.Context：`deps/build.sh` 把它们分别安装在 `deps/install/asan` 和 `deps/install/tsan`，CMake 按模式选用并核对路径。sanitizer 构建不链接 jemalloc，运行时无需 `LD_LIBRARY_PATH`；ASan 下 Boost.Context 的 makecontext 警告是已知提示。构建配置检查不等于 sanitizer 运行验证。第一方旧测试目标和 CTest 注册已移除，不再使用 `BUILD_TESTING` 开关。

回归工具在 [`regression/`](../regression)：`control_api.py --server-bin <ELF>` 启动服务端和签名桩，检查控制 API 的 JSON 校验（非法 JSON、缺失/未知/重复字段、数值范围、编码与转义、类型）与状态码；`protocol_matrix.py` 在回环地址上用 `bench/perf_run.py` 跑各输入、编码和输出组合及多源、慢读者、断流与鉴权故障场景，需要带性能客户端的构建目录、`avsync_720p30.flv`/`avsync_bframes.flv`/`avsync_whip.mkv` 夹具，Opus-in-FLV 场景另需支持它的 FFmpeg（已验证 7.1 不支持、9.0.1 支持）。结果写入 `--output` 下的 `summary.json`。

服务端 AAC 编码固定使用 libfdk-aac（关闭 afterburner）。FDK 许可不授予 AAC 专利许可，且要求分发二进制时随附其源码；该组合只用于内部部署，对外分发前需另行评估。

## Go 信令与模拟器

[`signaling/go.mod`](../signaling/go.mod) 要求 Go 1.26 或更新版本；SQLite 使用 modernc 驱动。Web 静态文件由 Go embed 打包，无独立 npm 构建步骤。

```sh
mkdir -p build-go
cd signaling
go build -o ../build-go/signaling .
go build -o ../build-go/gb_simulator ./simulator
go build ./...
go vet ./...
cd ..
```

模拟器保留为独立工具，参数由 [`signaling/simulator/config.go`](../signaling/simulator/config.go) 定义；本轮清理不将其当作生产媒体服务的一部分，也不附带运行设备验证。

## 启动与网络

本机默认参数可直接配对启动两个进程：

```sh
./build-prod/media_server --bind-address 127.0.0.1 \
  --webrtc-address 127.0.0.1 --threads 4 \
  --signaling-url http://127.0.0.1:9090

# 另一个终端；数据库目录必须可写。
./build-go/signaling -database ./signaling.db \
  -http-listen 127.0.0.1:9090 -sip-listen 127.0.0.1:5060 \
  -sip-advertise 127.0.0.1:5060 \
  -media-control-url http://127.0.0.1:8080 -media-ip 127.0.0.1
```

浏览器访问信令端口 `http://127.0.0.1:9090/`。媒体 HTTP 根路径不提供控制台。C++ 服务和 Go 服务接收 SIGINT/SIGTERM 并执行自身清理。

| 参数/资源 | 默认值与部署含义 |
| --- | --- |
| 媒体监听 | RTMP 1935/TCP、RTSP 8554/TCP、HTTP 8080/TCP；`--rtmp-port`、`--rtsp-port`、`--http-port` 可改 |
| 媒体地址 | `--bind-address` 与 `--webrtc-address` 默认回环，必须是明确 IP，不接受通配地址；跨机时设置可达地址 |
| 控制认证 | C++ `--control-token` 与 Go `-media-control-token` 一致；未设置时媒体控制和 verify 只允许回环来源 |
| 信令与 SIP | HTTP 9090/TCP、SIP 5060/UDP；SIP 宣告地址必须可被设备访问 |
| 公网媒体 URL | Go `-media-ip`、`-media-*-port`；HTTP 反向代理可用 `-media-http-origin` 指定 http/https origin |
| 媒体端口池 | `media_port_pool.h` 定义 49152–65535；RTP/RTCP、WebRTC 按会话使用，另有 GB TCP 显式监听端口 |
| UDP 媒体接收缓冲 | GB UDP、RTSP UDP 推流 RTP、WHIP、WHEP 拉流的接收 socket 申请 256 KiB `SO_RCVBUF`，内核按 `min(申请值, net.core.rmem_max) × 2` 生效：`rmem_max` 为常见默认 212992 时是 416 KiB，不小于 262144 时是 512 KiB。输出与 RTCP socket 用系统默认值 |

服务间控制使用 HTTP；需要 HTTPS/WSS 的客户端入口由部署网关终止 TLS。网关需保留媒体路径、Location、SDP 和 WebSocket upgrade，并允许长连接/chunked 传输。管理 API 和内部控制应按网络职责隔离；当前没有通用用户账户系统。

不同媒体进程应隔离媒体端口资源；端口池分配后 bind 失败即结束，不扫描重试。Go 会清理其不认识的 receiver，正式测量应使用独立 DUT 或工具的 existing 模式，避免与运行中的信令争夺所有权。设备 ID、域和 SIP 密码按现场配置；源码默认密码仅是本机默认值。

## Qt 桌面端

独立 [`desktop/CMakeLists.txt`](../desktop/CMakeLists.txt) 要求 CMake ≥3.22、C++20、Qt ≥6.2 Widgets/Network/WebSockets、Boost ≥1.89 头文件、SDL2 ≥2.0.20，以及 FFmpeg avformat/avcodec ≥60、avutil ≥58、swscale ≥7、swresample ≥4。

使用支持 MPEG-PS 解复用、Matroska 封装及所需 bitstream filter 的 FFmpeg shared SDK；对讲另需 pcm_alaw encoder。让 `PKG_CONFIG_PATH` 指向这个 SDK，避免误选系统旧库或服务端静态 SDK。

```sh
PKG_CONFIG_PATH="$FFMPEG_PREFIX/lib/pkgconfig" cmake -S desktop -B build-desktop \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-desktop --target gb_monitor -j4
./build-desktop/gb_monitor
```

`FFMPEG_PREFIX` 必须预先设置为实际 SDK 前缀。缺少 Qt WebSockets 时配置会失败，不能用仅 Qt Widgets 的安装声称桌面构建通过。客户端使用见 [`desktop/README.md`](../desktop/README.md)。

`cmake --install build-desktop --prefix <目录>` 只安装程序。内部运行包由 `desktop/package_internal_runtime.py` 生成，先查看其 `--help`，提供构建目录、FFmpeg 前缀和源码、匹配的 Qt 插件目录及不存在的输出目录。工具收集依赖/许可材料，不包含宿主驱动和所有动态加载模块，也不代表通用 Linux 二进制兼容。媒体性能包见 [performance.md](performance.md)。

## 维护约定

第一方 C++ 以 `.clang-format` 为格式依据：snake_case、传统 include guard、4 空格、Allman、150 列。include 按标准/系统、第三方、项目、必要的 `extern "C"` 分组，组内按路径长度排列；不开启自动 include 排序。类的私有函数和成员变量分组，保留 owner 上的 startup/shutdown 语义。只改任务涉及的代码，不顺带整理第三方。

提交使用简短中文动宾标题，按逻辑范围精确暂存，执行 `git diff --check`；提交和推送须服从本次任务授权。构建、静态检查和媒体运行验证分别报告，历史结果不能代替当前验证。原有设计取舍和经验保存在 `.agent/`。
