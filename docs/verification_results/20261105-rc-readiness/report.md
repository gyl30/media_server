# 四项任务与 RC 就绪验收（2026-11-05）

BASE_HEAD `2adfaea`（与 origin/main 相同，开始时工作区干净）。

- 第一轮的工具、CI、文档和打包改动分为 `ca131fc`、`5977442`、`0b9a3bd` 三个提交，按用户授权推送。
- 推送后首次真实 CI 运行发现一个仓库原有的构建缺陷，由 `f43bf43` 修复。
- CI 工作流自身的格式检查缺陷在本报告所在的提交中修复。

RC 候选为 `f43bf43`，生产源码相对 `2adfaea` 只改了 `build_info.cpp` 的一行 include。

## 结论

| 项 | 结论 | 依据 |
| --- | --- | --- |
| A WHEP 音频验收判据 | GO | 确定性 RED→GREEN（旧判据 14 个用例中 7 个与期望不符，新判据 14/14 符合），合成用例 12/12，RC 二进制上的完整协议矩阵（第 4 节） |
| B 真实摄像机 | GO | 用户提供账号并授权后，在实机上完成注册、目录、点播、WHEP/RTSP 观看、对讲上行与设备实际播放、占用、停止、注销与恢复、断网、信令重启；配置已恢复，前后比对无差异（第 2 节） |
| C CI | 见第 3 节 | 首次真实运行暴露了构建缺陷（已修复，修复后 media-server 作业通过）和工作流格式检查缺陷（已修复，等待修复后的运行结果） |
| BUILD | GO | 干净克隆 `f43bf43`，依赖从源码包重新构建，`--version` 为 `f43bf436d8e0`，不带 `-dirty` |
| PACKAGE | GO（仅限内部部署） | 包与 SHA256 校验，隔离 netns 冒烟 25/25。对外分发受许可证约束，见第 5 节 |
| DEVICE GO | 成立 | 第 2 节 |
| RC GO | 取决于 CI | 见文末“最终状态” |

## 1. 上下文核对

- HEAD、origin/main 和 `git ls-remote origin main` 都是 `2adfaea28ad2c517d3f2947439e53c0dc860b01f`。
- 依赖：GCC 16.0.1 20260315（ubuntu-toolchain-r/test PPA，jammy）；`deps/sources.txt` 固定 OpenSSL 3.5.9、Boost 1.92.0、jemalloc 5.3.0；子模块 FFmpeg n9.0.1、opus v1.6.1、fdk-aac v2.0.3、libsrtp v2.7、ada v4.0.0、simdjson v4.6.11、spdlog v1.17.0；ireader 源码在仓库内；Go 1.27.3（`go.mod` 要求 ≥1.26）。
- 已遵守的既定决策：`stream_id` 不复用、WHIP/WHEP 架构、不恢复 GOP/history、不新增生命周期状态、不用异常、不恢复 UDP GSO、libsrtp 策略不变、RTSP AAC 维持 KEEP/NO-GO、不改 `third/**`、不调整 sysctl、不创建 Issue。

## 2. 任务 B：真实摄像机

第一轮因为没有设备账号而 BLOCKED。随后用户提供了设备 Web 账号，并授权通过设备 Web 端修改配置，本节为第二轮实测。

**环境**

- 摄像机：大华 DH-SD-P3A1205-P，固件 2.811.0000007.3.R（2021-11-16），与 [20261023](../20261023-e12340d-gb-camera/results.md) 是同一台。用户最初给出的另一个地址无响应，按用户更正改用之前的地址。
- 平台：RC 包中的 `media_server` 和 `signaling`（`2adfaea28ad2`），在测试机上用 20261023 的 `platform.sh` 启动；该脚本只去掉了运行库路径，因为 RC 是静态链接。
- 观看端：本机 headless Chrome（Playwright）通过 Web 控制台播放；测试机上 ffmpeg 作为 RTSP 观看端。

**配置变更与恢复**

- 变更前先导出全部配置（16529 行）。槽位 0：GB28181，指向另一个平台，`IsOnLine=false`。槽位 1：`serverOption=Disable`，原 SIP 服务器 `<ORIGINAL_SIP_SERVER>:5060`。只修改槽位 1，槽位 0 全程未动。
- 测试期间槽位 1 改为 GB28181，指向测试平台的 15060 端口。设备保存的 SIP 密码与平台不一致，摄像机的 REGISTER 每次都被 401 拒绝；CGI 读出的密码是掩码，无法得知原值。经用户确认后，把槽位 1 的密码设为平台使用的测试密码。
- 恢复：槽位 1 改回 `Disable`、原 SIP 服务器和 5060 端口。完整配置前后比对（排除在线状态和时间类字段）无差异。密码字段是掩码，比对不出变化，测试密码保留在槽位 1。恢复后 130 s 内测试机没有再收到摄像机报文。
- 设备账号只临时存放在测试机上权限 0600 的文件里，测试结束后已删除，没有写入仓库或报告。

**结果**

| 项 | 结果与证据 |
| --- | --- |
| 注册 | 平台添加设备前，REGISTER 返回 403；添加后返回 401，Digest 密码不一致时继续 401；改设密码后 REGISTER 401 → 200，3 s 内平台显示在线 |
| 目录 | 返回 1 个通道 `IP PTZ Camera`，状态 ON |
| 点播 | API 点播 0.7 s 返回播放地址；Web 控制台显示“点播中” |
| Chrome WHEP | H.264 4d001f 1920×1080 25 fps，12 s 内解码 332 帧、7 个关键帧，0 丢包；PCMA 8 kHz 音轨持续收包，0 丢包；页面状态“播放中” |
| RTSP 观看端 | ffmpeg 20 s：H.264 1920×1080 解码 500 帧，PCMA 8 kHz 单声道 333 帧，解码 0 错误；平均音量 −41.6 dB |
| 并发观看 | 对讲测试期间，WHEP 页面与 RTSP 录音同时观看同一 live |
| 对讲上行 | Web 控制台“开始对讲” → “正在上送麦克风音频”；平台发往摄像机的 PCMA RTP 共 1328 包 × 160 字节，持续 26.5 s，序号无缺口，时间戳步进 160；解码后预声明的五频音序依次检出（连续 66 段） |
| 设备实际播放 | 同时录制摄像机麦克风回传的音轨：对讲前 RMS 约 30–200，对讲期间约 6400–17000，结束后回到约 20–140；五个频点按源顺序出现 |
| 停止对讲 | UI 停止后 talk 返回 404 |
| 对讲占用 | 已有对讲时再次创建，返回 409 `talk_busy`；只创建、不上传的对讲由平台按期限自动回收 |
| 停止点播 | 确认框默认焦点为“取消”；确认后 receiver 列表为空，媒体进程 UDP 套接字为 0 |
| 设备注销与恢复 | 播放中把槽位 1 停用：3.6 s 后平台离线，receiver 释放；重新启用后 4 s 上线，目录恢复 |
| 异常断网 | 测试机用临时 nft 规则丢弃摄像机入站报文（测完已删除）：约 22 s 后 receiver 空闲超时，播放器显示“已结束”；90 s 心跳超时后设备离线，receiver 与 UDP 套接字全部释放；网络恢复 27 s 后摄像机重新注册，点“重试”后重新出画（52 帧，0 丢包） |
| 信令重启 | 平台重启后摄像机在 3 分 36 s 后重新注册上线（20261023 为 3 分 40 s）；旧 receiver 已清理 |
| 资源 | 每次停止或清理后 receiver 为空、媒体进程 UDP 套接字为 0；停止后端口 15060、38470–38473 全部释放，媒体日志没有 error 行 |

**观察（未修改）**

- 摄像机 REGISTER 的 Digest 不正确时，平台持续返回 401，没有异常。
- 一次点播里出现 `stream authorization rejected reason=token_not_found`：同一个一次性播放 token 被第二次使用而被拒绝，播放本身未受影响。没有进一步定位是哪个页面组件发起的第二次请求。
- 本节使用的是 `2adfaea` 的 RC 二进制。其后 `f43bf43` 只修改了 `build_info.cpp` 的头文件引用（见第 3 节），对运行行为没有影响。

## 3. 任务 C：CI

见 [`.github/workflows/ci.yml`](../../../.github/workflows/ci.yml) 和 [build-deploy.md](../../build-deploy.md)。

- runner 用 ubuntu-22.04，与开发机及测试机的 glibc 2.35 相同（runner-images 当前仍列出 22.04）；GCC 16 来自与开发机同一个 PPA。
- 权限：`contents: read`；checkout 不保留凭据；并发组按 ref 划分，新运行会取消旧运行。
- 第三方 Action 固定到提交：checkout v7.0.1 `3d3c42e`、cache v6.1.0 `55cc834`、setup-go v7.0.0 `b7ad1da`。
- 缓存：`deps/install` 的键包含编译器版本和 `sources.txt`/`build.sh` 的摘要；ccache 的键包含编译器和构建参数，pull request 只读。不缓存密钥或设备数据。
- 没有放进 CI 的项目：协议矩阵、容量、长稳、真实设备、需要 netns 的 UDP 背压检查、依赖真实 dump 的 WHEP 回放检查，以及 RTSP AAC 时间线检查（它的结果本身就是已知 FAIL，作为 CI 门禁需要另行定义期望输出）。

**真实运行**

| 运行 | 提交 | 结果 |
| --- | --- | --- |
| 41986567372 | `0b9a3bd` | checks、signaling 通过；media-server 构建失败：`build_info.cpp:5: fatal error: srtp2/srtp.h: No such file or directory` |
| 41990061189 | `f43bf43` | media-server 通过（依赖缓存命中，5 分 41 秒：构建、版本串、动态依赖、控制 API、4 个 C++ 检查）；signaling 通过；checks 在格式检查步骤失败 |

**缺陷一：构建依赖系统 libsrtp 头文件（仓库原有）**

- 原因：`build_info.cpp` 写的是 `#include <srtp2/srtp.h>`，而仓库内 libsrtp 只在 `include/` 下提供 `srtp.h`。开发机装有系统包 `libsrtp2-dev` 2.4.2，这条路径实际解析到 `/usr/include/srtp2/srtp.h`，所以本机构建一直能过；runner 上没有这个包，构建失败。
- 证据：用该目标的实际编译参数加 `-H` 预处理，修复前解析到 `/usr/include/srtp2/srtp.h`，修复后解析到 `third/libsrtp/include/srtp.h`。扫描全部编译单元的依赖文件，除 glibc 自身外，引用系统开发包头文件的只有这一处。
- 修复：`f43bf43` 改为与 `srtp_transport.cpp` 一致的 `<srtp.h>`。版本串中的 `libsrtp2 2.7.0` 来自链接进来的库函数，修复前后相同。
- 影响：第一轮 RC（`2adfaea`）的 `build_info.cpp` 是用 2.4.2 的头文件编译的。只用到了 `srtp_get_version_string()` 的声明，两个版本的声明相同，运行行为不受影响；但不满足“只依赖锁定依赖”的要求，所以 RC 用 `f43bf43` 重建。

**缺陷二：工作流的格式检查误判（本轮引入）**

- 原因：`git clang-format --diff BASE HEAD` 在没有格式问题时输出为空、退出码为 0，而工作流只把 “no modified files to format” 和 “clang-format did not modify any files” 当作通过，空输出被判为失败。第一次运行的基准提交没有 C/C++ 改动，输出是 “no modified files”，所以没有暴露。
- 本地重放的证据也有错误：第一轮报告写的“在 `830668b..HEAD` 上试跑结果为空”，实际是本机没有安装 `git clang-format` 子命令，命令根本没有执行。
- 修复与验证：空输出也视为通过。用 pip 包自带的 `git-clang-format` 抽取工作流中的实际脚本重放：`0b9a3bd..f43bf43` 和 `2adfaea..f43bf43` 都返回 0；在临时 worktree 中提交一个故意改坏格式的 include，返回 1 并输出差异。

**本机逐项重放**（第一轮，干净克隆 `2adfaea` 加工作流）：

| 作业 | 命令 | 结果 |
| --- | --- | --- |
| checks | `git diff --check`（除 `third/`、`docs/` 外的整树） | 无输出 |
| checks | `python3 -m compileall bench regression docs/verification_results` | 通过 |
| checks | `whep_timeline_check.py` | 12/12 |
| signaling | gofmt / go vet / go test / 两个 go build | 通过；仓库目前没有 Go 测试文件，`go test` 只完成编译 |
| media-server | 开启 benchmarks 的 RelWithDebInfo 构建 | 2 分 05 秒（本机 -j8）；能通过是因为本机有系统 libsrtp 头文件，见缺陷一 |
| media-server | 版本串、`ldd` | `2adfaea28ad2`；只依赖 libc/libm |
| media-server | `control_api.py` | 76/76 |
| media-server | port_pool / rtp_ssrc 20000×20 / srtp_profile / udp_transport ordering | 4 项都 PASS |

本机重放没能发现缺陷一，因为开发机环境比 runner 多了系统包。这也说明 CI 必须以真实运行为准。用 actionlint 做静态检查需要下载外部代码，被本机权限策略拒绝，没有运行。

## 4. 任务 A：现场回归（RC 二进制 + 新判据）

作业脚本：[`jobs/a01-matrix.sh`](jobs/a01-matrix.sh)。

- 服务端：第一轮 RC 包中的 `media_server`（`2adfaea28ad2`）。`f43bf43` 只改了版本串所用头文件的路径，没有重跑矩阵。
- 客户端：原有 benchmark 构建；把本轮修改的 `perf_run.py`、`whep_dump_av.py`、`protocol_matrix.py` 换入运行包 `rt-a`，其余文件与 `rt-r` 逐一比对 sha256，完全相同。
- 运行环境：测试 netns，`--workers 4`。

**结果**

- 35 个场景：32 PASS，3 XFAIL（`rtsp-tcp-h264-aac`、`rtsp-udp-h264-aac`、`rtsp-tcp-h265-aac`）。没有 FAIL，也没有 XPASS。
- 3 个 XFAIL 仍然只由 GB 输出的 RTSP AAC 锯齿造成。

**WHEP 探针**

- 24 个带 WHEP 探针的场景全部 `verdict: PASS`，覆盖 AAC/Opus/PCMA/PCMU 源、WHIP、Pull、GB 输入、Chrome offer 和多源跨 worker。每个场景的 `audio_rtp_lost`、`audio_pts_gaps`、`audio_pts_overlaps` 都是 0，音频包为 1171–1267 个（`fault-source-stop` 中途停源，为 1057 个）。
- 录制侧伪影在本轮出现了 2 次：`rtmp-h264-aac` 和 `multi-source-cross-worker` 的 `recorded_audio_pts_gaps` 为 1。按 RTP 判定都是连续的，这正是第一轮定位的那个伪影，旧字段此时会误报。
- `source_continuous_seconds` 为 110.1–116.2 s，远大于 25 s 的判定窗口，因此全部音频包都在判定范围内。
- RTSP AAC 的 3 个场景里，WHEP 输出的音频 RTP 是连续的：锯齿没有传到 AAC→Opus egress 的输出。所以把音频判据接入 `ok` 不会改变这 3 个 XFAIL。

**清理**：所有场景的剩余 receiver 都为空；清理后 FD 都回到 20（基线为 21–25）。

## 5. 任务 D：RC 构建、打包与许可证

**构建**（脚本流程：克隆 → 子模块取自本地仓库对象 → `deps/build.sh` → CMake → Go）

| 项 | 值 |
| --- | --- |
| 源码 | `f43bf436d8e05d479caa42cd513536131493a014`，构建后 `git status --porcelain` 为空 |
| CMake | `-DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=gcc-16 -DCMAKE_CXX_COMPILER=g++-16`，HARDENING=ON，SANITIZER=none，BENCHMARKS=OFF |
| 依赖 | BUILD-INFO inputs `313880fd…`，与第一轮相同；构建耗时 1 分 51 秒（源码包来自本地缓存，经 sha256 校验） |
| `media_server --version` | `media_server f43bf436d8e0 compiler 16.0.1 20260315 … boost 1_92 OpenSSL 3.5.9 ffmpeg n9.0.1 libopus 1.6.1 libsrtp2 2.7.0 allocator jemalloc 5.3.0` |
| media_server | sha256 `acf5fdab6722b4786a085c7f7238f4c0fc7e10e86b1ac0a604fbe15355e96fd3`，build-id `e5f0b23ef02dbd8a20109a0d538fc095070342d0`；`build_info.cpp` 的依赖文件只引用 `third/libsrtp/include/srtp.h` |
| signaling | `CGO_ENABLED=0 go build -trimpath`，sha256 `f3008e1f658cf0b86558b03a14ab7d72d6ee7832d57a4b0735bcd85d44c7f524`，静态链接 |
| 运行时 | 只依赖 libc.so.6、libm.so.6 和 ld-linux；无 RPATH/RUNPATH；PIE、BIND_NOW、RELRO；GNU_STACK 不可执行；最高要求 GLIBC_2.35（第一轮检查，构建参数未变） |
| FFmpeg 许可模式 | `FFMPEG_LICENSE "LGPL version 2.1 or later"`，未开启 gpl/nonfree |

第一次 CMake 构建中，GCC 16 在编译 FFmpeg 的 `libavcodec/itut35.o` 时段错误退出（139）。在同一目录增量重跑后通过，没有复现。这个编译器是 PPA 提供的 experimental 快照，这次崩溃记为环境问题。

第一轮 RC（`2adfaea`）：media_server sha256 `d4319ece…f7fa`，包 sha256 `67518ca4…3708`。因缺陷一已作废，测试机上的副本已删除。

**打包**：[`package_rc.sh`](package_rc.sh)

- 产物：`media_server-f43bf436d8e0.tar.gz`，sha256 `d487714b39346fd961d368d65fd0292856293e3e5034bcfb0a753f14254a86d9`，约 99 MB，89 个条目。
- 包内内容：`bin/media_server`、`bin/signaling`、`BUILD-INFO`（版本串、提交、子模块、依赖摘要、CMake 参数、Go 版本、build-id）、`SHA256SUMS`，以及 `licenses/` 下 C/C++ 依赖、Go 标准库和 17 个 Go 模块的许可证文本。
- tar 的属主统一为 0，mtime 取提交时间，条目按名称排序。
- 拒绝条件已实测（第一轮）：源码树有改动时退出 1；`--version` 与 `git describe` 不符时退出 1（用 `830668ba4b8f-dirty` 的旧构建验证）。这保证不能把历史二进制改名充当 RC。

**安装与冒烟**：在测试机隔离目录中解包，`sha256sum -c` 通过；在测试 netns（只有回环与 198.19.0.10/11）里运行 [`smoke.sh`](smoke.sh)，25/25 通过：

- 参数拒绝：`--threads 0`、通配地址、未知选项都退出 1；
- 首行日志与 `--version` 一致；
- 5 个监听端口（RTMP/RTSP/HTTP/信令 HTTP/SIP UDP）；
- 控制 API：带 token 返回 200，缺 token 或 token 错误返回 403；receivers 为空；
- 信令：Web 控制台、`/api/devices` 和 SQLite 文件；
- SIGTERM 后两个进程都以 0 退出，5 个端口全部释放，没有残留进程，媒体日志没有 error 行。

初版冒烟脚本把鉴权失败写成期望 401，与服务端实际返回的 403 不符，2 项失败。核对 `http_session.cpp:134` 后修正了期望值，没有修改服务端。

服务端没有配置文件，配置全部来自命令行参数；信令的持久状态是 `-database` 指定的 SQLite 文件。

**许可证审查**

| 组件 | 许可证 | 链接方式 | 分发时的要求 |
| --- | --- | --- | --- |
| FFmpeg n9.0.1（avformat/avcodec/swresample/avutil） | LGPL 2.1+ | 静态 | 提供 FFmpeg 对应源码与构建配置；静态链接时还须让接收方能用修改后的库重新链接（提供应用目标文件或源码），并附许可证文本 |
| fdk-aac v2.0.3 | FDK AAC 许可 | 静态 | 文档中保留完整许可文本，免费提供 FDK 源码；**不授予 AAC 专利许可**（需另行通过 Via LA 等取得）；不得用 Fraunhofer 名义背书 |
| OpenSSL 3.5.9 | Apache-2.0 | 静态 | 附许可证 |
| Boost 1.92 | BSL-1.0 | 静态 | 二进制分发不强制，已附 |
| jemalloc 5.3.0 | BSD-2 | 静态 | 文档中保留版权声明 |
| opus 1.6.1、libsrtp 2.7 | BSD-3 | 静态 | 保留声明 |
| ada、simdjson | Apache-2.0 / MIT 双许可 | 静态 | 保留声明 |
| spdlog、内置 fmt、ireader（media-server/avcodec） | MIT | 静态 | 保留声明 |
| libstdc++/libgcc | GPL-3 加 GCC 运行库例外 | 静态 | 符合例外条件，无额外义务 |
| Go 标准库与 17 个模块（sipgo、pion/sdp、modernc sqlite/libc 等） | BSD/MIT；SQLite 为公有领域 | 静态 | 保留声明，已全部收录 |

需要决策的发布阻断项（不属于缺陷）：

1. **仓库没有本项目自身的 LICENSE**，第一方代码的分发授权不明确。
2. **FDK-AAC 不含专利许可**，加上 LGPL 静态链接的重新链接义务，与 build-deploy.md 已有的结论一致：当前组合只适合内部部署，对外分发前需要法务评估，或改为可分发的 AAC 编码方案。
3. `third/ireader/README.md` 写明代码“copied from the user-provided SimpleMediaServer reference package”，上游许可为 MIT，来源链需要确认。
4. 二进制带调试信息，`FFMPEG_CONFIGURATION` 字符串中含有构建目录的绝对路径。对外发布时应考虑剥离调试信息并使用固定的构建路径。

## 6. 性能与生命周期

生产代码只改了 `build_info.cpp` 的一行 include，不影响运行路径。本轮没有重新做容量或长稳测量。完整协议矩阵（第 4 节）和实机测试（第 2 节）覆盖了源停止、慢读者、鉴权失败、churn、设备注销、断网和信令重启等生命周期路径；每次清理后 receiver、FD 和 UDP 套接字都已回收。

## 7. 未解决问题

- C：Go 侧没有单元测试；RTSP AAC 时间线检查没有进入 CI。
- B：槽位 1 的 SIP 密码已改为测试密码，原值无法读出，因此无法恢复；其余配置已恢复。
- 许可证：第 5 节列出的 4 个发布阻断项。
- 测试机证据丢失：`runs11/s3-*` 和 `runs11/s5-probe` 的媒体文件被旧作业库的启动清理删除，已在相关报告中更正。之后的检查都不再引用旧作业库。

## 最终状态

- BUILD GO、PACKAGE GO（仅限内部部署）、DEVICE GO。
- 修复缺陷一后，media-server 作业的全部步骤和 signaling 作业已在 GitHub 上真实通过（41990061189）。
- checks 作业的格式检查缺陷在本报告所在的提交中修复。CI GO，以及由此得出的 RC GO（RC 为 `f43bf43`，本提交只改了工作流和文档），以该提交触发的那次运行全部通过为准。
- 对外分发仍是 NO-GO，原因见第 5 节的许可证阻断项。
