# 四项任务与 RC 就绪验收（2026-11-05）

BASE_HEAD `2adfaea`（与 origin/main 相同，开始时工作区干净）。本轮没有修改生产代码，RC 候选的生产源码就是 `2adfaea`。改动只涉及测试工具、CI、文档和打包脚本，全部未提交。

## 结论

| 项 | 结论 | 依据 |
| --- | --- | --- |
| A WHEP 音频验收判据 | GO | 确定性 RED→GREEN（旧判据 14 个用例中 7 个与期望不符，新判据 14/14 符合），合成用例 12/12，RC 二进制上的完整协议矩阵（第 4 节） |
| B 真实摄像机 | BLOCKED | 没有设备账号，CGI 均返回 401；专用槽位、原始配置和其他平台注册都无法只读确认（第 2 节） |
| C CI | BLOCKED（未在 GitHub 运行） | 工作流已写好，各作业的命令在本机干净克隆上逐项重放并通过；要在 GitHub 上真实运行需要推送分支，本轮没有推送授权 |
| BUILD | GO | 干净克隆 `2adfaea`，依赖从源码包重新构建，`--version` 为 `2adfaea28ad2`，不带 `-dirty` |
| PACKAGE | GO（仅限内部部署） | 包内容与 SHA256 校验、在隔离 netns 中安装后冒烟 25/25、RC 二进制跑完整协议矩阵。对外分发受许可证约束，见第 5 节 |
| CI GO | 不成立 | 未取得真实执行结果 |
| DEVICE GO | 不成立 | BLOCKED |
| RC GO | 不成立 | CI 与设备两项必需验收未完成 |
| 整体 | INCONCLUSIVE | 没有失败证据；阻断项是授权与凭据，不是缺陷 |

## 1. 上下文核对

- HEAD、origin/main 和 `git ls-remote origin main` 都是 `2adfaea28ad2c517d3f2947439e53c0dc860b01f`。
- 依赖：GCC 16.0.1 20260315（ubuntu-toolchain-r/test PPA，jammy）；`deps/sources.txt` 固定 OpenSSL 3.5.9、Boost 1.92.0、jemalloc 5.3.0；子模块 FFmpeg n9.0.1、opus v1.6.1、fdk-aac v2.0.3、libsrtp v2.7、ada v4.0.0、simdjson v4.6.11、spdlog v1.17.0；ireader 源码在仓库内；Go 1.27.3（`go.mod` 要求 ≥1.26）。
- 已遵守的既定决策：`stream_id` 不复用、WHIP/WHEP 架构、不恢复 GOP/history、不新增生命周期状态、不用异常、不恢复 UDP GSO、libsrtp 策略不变、RTSP AAC 维持 KEEP/NO-GO、不改 `third/**`、不调整 sysctl、不创建 Issue。

## 2. 任务 B：真实摄像机（BLOCKED）

只做了只读检查：

- 测试机到摄像机可以 ping 通（0.39 ms），TCP 80/554/37777 端口开放。
- `magicBox.cgi?action=getDeviceType` 返回 401（Digest）。
- 本机和测试机上都没有设备账号：仓库只跟踪了 `results.md`、脚本和夹具，测试机上 `gb-camera-e12340d-*` 目录里没有 `sip.private` 或 CGI 凭据。
- 被动抓包 70 s，没有收到摄像机的 UDP 报文，与上次恢复后的状态（槽位 1 为 Disable）一致。

因为没有账号，设备型号、专用槽位、原始平台配置、当前其他平台注册都无法确认，也无法给出完整的恢复方式。按任务边界没有修改任何设备配置，也没有用模拟器代替实机。

第三节的 12 项实机测试本轮都未执行。可以引用的只有 [20261023](../20261023-e12340d-gb-camera/results.md) 的历史结果。

要继续这一项，需要提供：设备账号（不写入仓库），并确认槽位 1 可以作为专用测试槽位、恢复为 `Disable` 和原 SIP 服务器即为完整恢复。

## 3. 任务 C：CI

见 [`.github/workflows/ci.yml`](../../../.github/workflows/ci.yml) 和 [build-deploy.md](../../build-deploy.md)。

- runner 用 ubuntu-22.04，与开发机及测试机的 glibc 2.35 相同（runner-images 当前仍列出 22.04）；GCC 16 来自与开发机同一个 PPA。
- 权限：`contents: read`；checkout 不保留凭据；并发组按 ref 划分，新运行会取消旧运行。
- 第三方 Action 固定到提交：checkout v7.0.1 `3d3c42e`、cache v6.1.0 `55cc834`、setup-go v7.0.0 `b7ad1da`。
- 缓存：`deps/install` 的键包含编译器版本和 `sources.txt`/`build.sh` 的摘要；ccache 的键包含编译器和构建参数，pull request 只读。不缓存密钥或设备数据。
- 没有放进 CI 的项目：协议矩阵、容量、长稳、真实设备、需要 netns 的 UDP 背压检查、依赖真实 dump 的 WHEP 回放检查，以及 RTSP AAC 时间线检查（它的结果本身就是已知 FAIL，作为 CI 门禁需要另行定义期望输出）。

本机逐项重放（干净克隆 `2adfaea` 加本轮工作流）：

| 作业 | 命令 | 结果 |
| --- | --- | --- |
| checks | `git diff --check`（除 `third/`、`docs/` 外的整树） | 无输出 |
| checks | `python3 -m compileall bench regression docs/verification_results` | 通过 |
| checks | `whep_timeline_check.py` | 12/12 |
| checks | clang-format 20.1.8 改动行检查 | 本轮没有改 C/C++，未触发；在 `830668b..HEAD` 上试跑结果为空 |
| signaling | gofmt / go vet / go test / 两个 go build | 通过；仓库目前没有 Go 测试文件，`go test` 只完成编译 |
| media-server | 开启 benchmarks 的 RelWithDebInfo 构建 | 2 分 05 秒（本机 -j8） |
| media-server | 版本串、`ldd` | `2adfaea28ad2`；只依赖 libc/libm |
| media-server | `control_api.py` | 76/76 |
| media-server | port_pool / rtp_ssrc 20000×20 / srtp_profile / udp_transport ordering | 4 项都 PASS |

用 actionlint 做静态检查需要下载外部代码，被本机权限策略拒绝，没有运行；YAML 解析通过。

## 4. 任务 A：现场回归（RC 二进制 + 新判据）

作业脚本：[`jobs/a01-matrix.sh`](jobs/a01-matrix.sh)。

- 服务端：RC 包中的 `media_server`（`2adfaea28ad2`）。
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

**构建**（脚本流程：克隆 `2adfaea` → 子模块取自本地仓库对象 → `deps/build.sh` → CMake → Go）

| 项 | 值 |
| --- | --- |
| 源码 | `2adfaea28ad2c517d3f2947439e53c0dc860b01f`，构建后 `git status --porcelain` 为空 |
| CMake | `-DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=gcc-16 -DCMAKE_CXX_COMPILER=g++-16`，HARDENING=ON，SANITIZER=none，BENCHMARKS=OFF |
| 依赖 | BUILD-INFO inputs `313880fd…`；构建耗时 1 分 56 秒（源码包来自本地缓存，经 sha256 校验） |
| `media_server --version` | `media_server 2adfaea28ad2 compiler 16.0.1 20260315 … boost 1_92 OpenSSL 3.5.9 ffmpeg n9.0.1 libopus 1.6.1 libsrtp2 2.7.0 allocator jemalloc 5.3.0` |
| media_server | sha256 `d4319ecea2a40044d8085d27277ed781c391e166c745f0f2ad2c54f4606df7fa`，build-id `ba06f290bee226247557246f531dab9e7f8ab8db`，281 MB（含调试信息） |
| signaling | `CGO_ENABLED=0 go build -trimpath`，sha256 `13cb89d5975ace60090bd116da8400d39321536696567bf062a68010a5a9b164`，静态链接；`vcs.revision=2adfaea…`，`vcs.modified=false` |
| 运行时 | 只依赖 libc.so.6、libm.so.6 和 ld-linux；无 RPATH/RUNPATH；PIE、BIND_NOW、RELRO；GNU_STACK 不可执行；最高要求 GLIBC_2.35 |
| FFmpeg 许可模式 | `FFMPEG_LICENSE "LGPL version 2.1 or later"`，未开启 gpl/nonfree |

**打包**：[`package_rc.sh`](package_rc.sh)

- 产物：`media_server-2adfaea28ad2.tar.gz`，sha256 `67518ca4ccc44a87d3a446ee1c6d121a2d535035528ea868e85f25fd4d3c3708`，99 MB，89 个条目。
- 包内内容：`bin/media_server`、`bin/signaling`、`BUILD-INFO`（版本串、提交、子模块、依赖摘要、CMake 参数、Go 版本、build-id）、`SHA256SUMS`，以及 `licenses/` 下 C/C++ 依赖、Go 标准库和 17 个 Go 模块的许可证文本。
- tar 的属主统一为 0，mtime 取提交时间，条目按名称排序。
- 拒绝条件已实测：源码树有改动时退出 1；`--version` 与 `git describe` 不符时退出 1（用 `830668ba4b8f-dirty` 的旧构建验证）。这保证不能把历史二进制改名充当 RC。

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

生产代码没有改动，RC 二进制与 `830668b`、`2adfaea` 的生产源码相同。本轮没有重新做容量或长稳测量。完整协议矩阵覆盖了源停止、慢读者、鉴权失败、churn 等生命周期路径，以及清理后的 receiver 和 FD 回收（第 4 节）。

## 7. 未解决问题

- B：缺少设备账号和配置授权。
- C：需要推送分支（或合并到 main）才能取得 GitHub Actions 的真实结果；Go 侧没有单元测试；RTSP AAC 时间线检查没有进入 CI。
- 许可证：第 5 节列出的 4 个发布阻断项。
- 测试机证据丢失：`runs11/s3-*` 和 `runs11/s5-probe` 的媒体文件被旧作业库的启动清理删除，已在相关报告中更正。之后的检查都不再引用旧作业库。
