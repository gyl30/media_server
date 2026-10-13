# 自有代码格式与提交消息审查

## 1. 结论

已检查 168 个第一方 C/C++ 文件。82 个文件需要 clang-format 排版，90 个文件需要 include 分组或长度排序，两阶段共涉及 118 个不同文件。两个代码阶段分别构建并运行全部 60 项 CTest，均通过；二次格式化稳定。

没有修改业务控制流、资源所有权、授权流程、命名、类布局或成员可见性。格式化器在一个测试文件的 8 处单语句 `if` 增加大括号，并在 3 处补 namespace 注释，均已单独核对。include 数量、拼写、分隔符和 C linkage 不变，没有新增或删除依赖。

最近 100 个提交有 2 项标题范围可以更具体的建议；没有确认必须拆分的无关逻辑混杂。不建议为这两项改写已发布历史，也不修改 Pi 使用的固定 SHA。格式与规范分支可在用户 Review 后合并，本轮不合并、不推送。

## 2. 基线与保护范围

```text
BASE_HEAD: 799455038c1dd82cfa30bd4b6ee5d9155748b3fe
FORMAT_HEAD: 09213ec16c68b03084322ca0edce1b82a60ec5f4
INCLUDE_HEAD: 5722ffb908f0e214d7d8898ca7e8ef6466a9aaa6
FINAL_HEAD: 本报告所在的“明确代码格式与提交消息规范”提交
branch: codex/code-style-cleanup
worktree: /var/tmp/media_server_code_style_20261013/worktree
review_date: 2026-10-13（Asia/Shanghai）
```

`FINAL_HEAD` 的准确 SHA 可在本分支运行 `git log -1 --format=%H -- docs/reviews/code_style_and_commit_audit.md` 获取；报告不能在不再次改写提交的情况下包含自己的提交 SHA，交付回复另列完整 SHA。

开始时已执行 fetch，主仓库为 main，HEAD 与 origin/main 相同且 clean。未使用主仓库 build，也未修改共享 Git 配置、子模块初始化状态或既有历史。

受保护的工作区：

- 主仓库 `/home/gyl/example/media_server`，main 基线 `7994550`。
- Pi 工作区 `/var/tmp/media_server_perf/20261011T151927Z-b4965b8-2737042/worktree`。
- Pi 阶段 6 工作区 `/var/tmp/media_server_perf_phase6_7788320/worktree`。
- Claude scratchpad `/tmp/claude-1000/-home-gyl-example-media-server/07f5432d-faf7-47e5-b2e2-dd6c2438cab2/scratchpad/base-src`。

没有连接 TEST、修改其 namespace、运行包和原始结果，也没有停止既有服务。第三方依赖在独立构建中只读使用主仓库已初始化的副本；没有执行 submodule init/deinit。独立 source 镜像仅把第一方路径链接到本 worktree，把 `third` 链接到主仓库对应路径，构建产物全部写入本轮独立 build。

## 3. 工具、配置与文件清单

实际工具：Ubuntu clang-format 20.1.8（`++20250708082409+6fb913d3e2ec-1~exp1~20250708202428.132`）。该版本可以解析现有全部配置；未修改 `.clang-format`、`.clang-tidy`、`.gitignore` 或 `.gitmodules`。

`.clang-format` 是唯一权威，实际关键设置如下：

| 设置 | 实际值 |
| --- | --- |
| Language / BasedOnStyle | Cpp / Google |
| IndentWidth / ColumnLimit | 4 / 150 |
| BreakBeforeBraces / PointerAlignment | Allman / Left |
| DerivePointerAlignment | false |
| SortIncludes / SortUsingDeclarations | false / false |
| InsertBraces / FixNamespaceComments | true / true |
| AllowShortBlocksOnASingleLine / AllowShortIfStatementsOnASingleLine | false / false |
| AllowAllArgumentsOnNextLine | true |
| BinPackParameters / BinPackArguments | false / false |
| SpacesBeforeTrailingComments | 4 |
| BraceWrapping.AfterCaseLabel | true |

完整配置在 `.clang-format` 和开发规范中。没有通过开启 `SortIncludes` 改用字母排序。

范围来自 tracked 第一方 `.c/.cc/.cpp/.cxx/.h/.hh/.hpp/.hxx` 清单，不包含 `third/**`、generated/vendor、build、Go 或其他 worktree。当前清单没有第一方 generated 文件需要例外排除。

| 目录 | 检查文件 | 排版修改 | include 修改 |
| --- | ---: | ---: | ---: |
| media | 120 | 51 | 52 |
| bench | 21 | 15 | 16 |
| tests | 22 | 14 | 21 |
| 根目录 | 5 | 2 | 1 |
| 合计 | 168 | 82 | 90 |

包含 include 的文件为 167 个；其余 1 个无 include，仍检查格式。本轮没有添加通用排序工具到仓库；使用本轮临时预览辅助，并逐项审查每个 include diff。

## 4. 阶段 A：clang-format

提交：`09213ec16c68b03084322ca0edce1b82a60ec5f4`，`统一自有C++代码排版`。

`git diff BASE_HEAD FORMAT_HEAD --stat`：82 个文件，1678 行增加、1475 行删除。主要差异是换行、缩进、调用参数排列与空白，不是函数或行为的增删。

验证：

- 168 个文件二次格式化均无变化。
- 全部 include 行的内容与顺序保持不变。
- 用 Clang 20 raw lexer 对 82 个修改文件比较非空白 token，并单独核对注释变化。
- 唯一执行 token 变化位于 `tests/webrtc_auth.cpp`：8 处单语句 `if` 增加成对大括号，内部仅赋值或调用 registry remove，无新增声明、作用域依赖或 `else` 绑定变化。
- `media/hls/hls.h`、`media/webrtc/whep.h` 补外层 namespace 注释；`tests/webrtc_sdp.cpp` 补匿名 namespace 注释。没有改注释含义。
- 其他文件的执行 token 不变；没有增加 helper、重命名或移动类成员。
- 完整构建、CTest 60/60 和 `git diff --check` 通过。

排版 diff 可复查：

```bash
git diff 799455038c1dd82cfa30bd4b6ee5d9155748b3fe 09213ec16c68b03084322ca0edce1b82a60ec5f4 -- media bench tests '*.cpp' '*.h'
```

## 5. 阶段 B：include

提交：`5722ffb908f0e214d7d8898ca7e8ef6466a9aaa6`，`统一自有头文件包含顺序`。

`git diff FORMAT_HEAD INCLUDE_HEAD --stat`：90 个文件，353 行增加、359 行删除。只移动现有 include 和组间空行。

全部 include 按标准库/系统、第三方、项目、`extern "C"` 分组。组内按路径字符长度升序，同长度保持原相对顺序。不强制 `.cpp` 自有头位于首行，不把 Boost、spdlog、OpenSSL 或 Ada 当作项目头。

单独核对的边界：

- `tests/accept_fault.c`、`tests/reset_peer.c` 的 `_GNU_SOURCE` 均保留在所有系统头之前，未跨越 feature-test 宏。
- 40 个 `extern "C"` include 区块保留原块边界和 C linkage，置于 include 区域最后；块内按相同长度规则整理。
- `bench/clients/rtmp_client.h` 的 `bench/packet_health.h` 移到已有 `struct rtmp_client_t;` 前，形成连续的 include 区域。该头不使用此 C 类型；前向声明本身和后续声明未改。
- 没有发现 include 区域中需要保留的条件编译排序例外。第三方头的已检查依赖由其自身 include 提供，完整重新构建没有暴露排序依赖。
- include 的多重集合（含 `<>`/`""`）、各 C-linkage 块的成员以及其余非 include 文本的非空白内容不变。
- 未确认缺失直接依赖，不混入头文件自包含修复。本轮不等价于对每个头做独立 translation unit 编译证明。
- 168 个文件再次格式化稳定；完整构建、CTest 60/60 和 diff check 通过。

无法安全整理的 include：**无已确认例外**。不存在为了排序增加、删除或替换依赖的提交。

include diff 可复查：

```bash
git diff 09213ec16c68b03084322ca0edce1b82a60ec5f4 5722ffb908f0e214d7d8898ca7e8ef6466a9aaa6 -- media bench tests '*.cpp' '*.h' '*.c'
```

## 6. 修改文件

以下是两阶段实际改变的 118 个文件，未列入的已检查文件无需修改：

| 文件 | 排版 | include |
| --- | --- | --- |
| `bench/clients/http_flv_client.cpp` | 是 | 是 |
| `bench/clients/http_flv_client.h` | 是 | 是 |
| `bench/clients/rtmp_client.cpp` | — | 是 |
| `bench/clients/rtmp_client.h` | — | 是 |
| `bench/clients/rtsp_client.cpp` | 是 | 是 |
| `bench/clients/webrtc_client.cpp` | 是 | 是 |
| `bench/clients/webrtc_client.h` | 是 | — |
| `bench/credential_file.h` | — | 是 |
| `bench/gb_ps_fanout.cpp` | — | 是 |
| `bench/http_flv_fanout.cpp` | 是 | — |
| `bench/measurement_start.h` | 是 | 是 |
| `bench/media_fanout.h` | 是 | 是 |
| `bench/media_progress.h` | 是 | 是 |
| `bench/packet_health.h` | — | 是 |
| `bench/rtmp_fanout.cpp` | 是 | — |
| `bench/rtsp_fanout.cpp` | 是 | — |
| `bench/rtsp_publisher.cpp` | 是 | 是 |
| `bench/webrtc_disconnect.cpp` | 是 | 是 |
| `bench/whep_fanout.cpp` | 是 | 是 |
| `bench/whip_publisher.cpp` | 是 | 是 |
| `config.cpp` | 是 | — |
| `media/core/media_stream.cpp` | 是 | 是 |
| `media/core/media_types.h` | 是 | — |
| `media/core/session_registry.cpp` | 是 | — |
| `media/core/session_registry.h` | — | 是 |
| `media/core/worker_sink_dispatcher.h` | 是 | 是 |
| `media/gb28181/gb28181_rtp_receiver.cpp` | 是 | 是 |
| `media/gb28181/gb28181_rtp_sender.h` | 是 | — |
| `media/gb28181/gb28181_tcp_receiver_session.cpp` | 是 | — |
| `media/gb28181/gb28181_tcp_receiver_session.h` | — | 是 |
| `media/gb28181/gb28181_tcp_sender_session.cpp` | 是 | — |
| `media/gb28181/gb28181_tcp_sender_session.h` | — | 是 |
| `media/gb28181/gb28181_udp_receiver_session.cpp` | 是 | 是 |
| `media/gb28181/gb28181_udp_receiver_session.h` | — | 是 |
| `media/gb28181/gb28181_udp_sender_session.cpp` | — | 是 |
| `media/gb28181/gb28181_udp_sender_session.h` | — | 是 |
| `media/hls/hls.cpp` | 是 | — |
| `media/hls/hls.h` | 是 | — |
| `media/hls/hls_play_session.cpp` | 是 | 是 |
| `media/hls/hls_play_session.h` | 是 | — |
| `media/hls/hls_segmenter.cpp` | — | 是 |
| `media/hls/hls_segmenter.h` | 是 | — |
| `media/http/gb28181_http.cpp` | 是 | 是 |
| `media/http/gb28181_http.h` | — | 是 |
| `media/http/gb28181_json.cpp` | 是 | — |
| `media/http/gb28181_json.h` | — | 是 |
| `media/http/hls_http_session.cpp` | 是 | 是 |
| `media/http/hls_http_session.h` | 是 | 是 |
| `media/http/http_flv_session.cpp` | 是 | 是 |
| `media/http/http_flv_session.h` | — | 是 |
| `media/http/http_session.cpp` | 是 | 是 |
| `media/http/http_target.cpp` | — | 是 |
| `media/http/rtsp_pull_http.cpp` | 是 | 是 |
| `media/http/rtsp_pull_http.h` | — | 是 |
| `media/http/signaling_verify.cpp` | 是 | 是 |
| `media/http/whep_http.cpp` | 是 | — |
| `media/http/whep_http.h` | — | 是 |
| `media/http/whip_http.cpp` | 是 | — |
| `media/http/whip_http.h` | — | 是 |
| `media/net/media_port_pool.h` | — | 是 |
| `media/net/tcp_listener.cpp` | 是 | — |
| `media/net/tcp_transport.cpp` | 是 | 是 |
| `media/net/tcp_transport.h` | — | 是 |
| `media/net/udp_transport.cpp` | 是 | — |
| `media/net/udp_transport.h` | — | 是 |
| `media/net/worker_context.cpp` | 是 | — |
| `media/net/worker_context.h` | 是 | 是 |
| `media/ps/mpeg_ps_output.cpp` | 是 | 是 |
| `media/ps/mpeg_ps_output.h` | — | 是 |
| `media/rtmp/rtmp_play_session.cpp` | 是 | — |
| `media/rtmp/rtmp_play_session.h` | — | 是 |
| `media/rtmp/rtmp_publish_session.cpp` | 是 | 是 |
| `media/rtmp/rtmp_session.cpp` | — | 是 |
| `media/rtsp/rtsp_play_session.cpp` | 是 | 是 |
| `media/rtsp/rtsp_play_session.h` | 是 | 是 |
| `media/rtsp/rtsp_publish_media.cpp` | 是 | — |
| `media/rtsp/rtsp_publish_session.cpp` | 是 | 是 |
| `media/rtsp/rtsp_publish_session.h` | — | 是 |
| `media/rtsp/rtsp_publish_udp_session.cpp` | 是 | 是 |
| `media/rtsp/rtsp_pull_media.cpp` | 是 | 是 |
| `media/rtsp/rtsp_pull_session.cpp` | 是 | — |
| `media/rtsp/rtsp_pull_session.h` | — | 是 |
| `media/rtsp/rtsp_sdp.cpp` | 是 | — |
| `media/rtsp/rtsp_server_connection.cpp` | 是 | 是 |
| `media/rtsp/rtsp_uri.cpp` | 是 | — |
| `media/webrtc/webrtc_packetizer.cpp` | 是 | — |
| `media/webrtc/webrtc_sdp.cpp` | 是 | — |
| `media/webrtc/whep.cpp` | — | 是 |
| `media/webrtc/whep.h` | 是 | 是 |
| `media/webrtc/whep_audio_egress.cpp` | 是 | — |
| `media/webrtc/whep_audio_egress.h` | 是 | 是 |
| `media/webrtc/whep_session.cpp` | 是 | 是 |
| `media/webrtc/whep_session.h` | 是 | 是 |
| `media/webrtc/whip.h` | — | 是 |
| `media/webrtc/whip_session.cpp` | 是 | 是 |
| `media/webrtc/whip_session.h` | 是 | 是 |
| `service.cpp` | 是 | 是 |
| `tests/accept_fault.c` | — | 是 |
| `tests/gb28181_ps_receiver.cpp` | 是 | 是 |
| `tests/gb_sender_lifecycle.cpp` | — | 是 |
| `tests/gb_tcp_lifecycle.cpp` | — | 是 |
| `tests/hls_segmenter.cpp` | 是 | 是 |
| `tests/http_flv_auth.cpp` | — | 是 |
| `tests/media_lifecycle.cpp` | 是 | 是 |
| `tests/media_port_pool.cpp` | 是 | 是 |
| `tests/protocol_play_auth.cpp` | 是 | 是 |
| `tests/rtsp_publish.cpp` | 是 | 是 |
| `tests/rtsp_pull_config.cpp` | — | 是 |
| `tests/rtsp_pull_shutdown.cpp` | 是 | 是 |
| `tests/session_start_stop.cpp` | 是 | 是 |
| `tests/signaling_verify.cpp` | 是 | 是 |
| `tests/tcp_transport.cpp` | 是 | 是 |
| `tests/udp_transport.cpp` | 是 | 是 |
| `tests/webrtc_auth.cpp` | 是 | 是 |
| `tests/webrtc_sdp.cpp` | 是 | 是 |
| `tests/whep_dtls_timeout.cpp` | — | 是 |
| `tests/whep_lifecycle.cpp` | — | 是 |
| `tests/worker_sink_fanout.cpp` | 是 | 是 |

## 7. 构建与回归

构建环境：GCC 16.0.1，C++23，Boost 1.92 静态库，FFmpeg SDK `/home/gyl/ffmpeg901`，RelWithDebInfo，BUILD_TESTING 与 MEDIA_SERVER_BUILD_BENCHMARKS 均开启。

```bash
PKG_CONFIG_PATH=/home/gyl/ffmpeg901/lib/pkgconfig \
cmake -S /var/tmp/media_server_code_style_20261013/build-source \
  -B /var/tmp/media_server_code_style_20261013/build \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=/usr/bin/gcc-16 \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++-16 \
  -DBoost_DIR=/usr/local/lib/cmake/Boost-1.92.0 \
  -DBoost_USE_STATIC_LIBS=ON \
  -DBUILD_TESTING=ON \
  -DMEDIA_SERVER_BUILD_BENCHMARKS=ON

cmake --build /var/tmp/media_server_code_style_20261013/build -j8
ctest --test-dir /var/tmp/media_server_code_style_20261013/build --output-on-failure -j1
```

| 阶段 | 完整构建 | CTest | diff check | 二次格式化 |
| --- | --- | --- | --- | --- |
| A 排版 | PASS | 60/60，284.09 秒 | PASS | 168/168 稳定 |
| B include（最终代码） | PASS | 60/60，283.51 秒 | PASS | 168/168 稳定 |
| C 规范文档 | 不改变编译输入，沿用 B 结果 | 沿用 B 全量结果 | PASS | 不适用 |

测试包括 transport、worker/session 生命周期、RTSP/RTMP/WebRTC、HLS、GB sender/receiver、授权、accept 恢复以及已注册的工具验收测试。工具测试是现有 CTest，不是正式容量、超载或长期性能测试。

未运行 ASan、UBSan、Go test 或额外大规模性能测试：没有修改业务或 Go，词法检查及两次完整构建/CTest 对本轮格式范围提供回归证据。未将未执行项记为 PASS。

原始日志和词法/include 清单仅保存在 `/var/tmp/media_server_code_style_20261013/evidence/`，不提交原始构建产物或日志。

## 8. Git 提交消息审查

固定范围：从 `799455038c1dd82cfa30bd4b6ee5d9155748b3fe` 逆向最近 100 个提交，最早为 `1d28c768cecc1be70460e5dee37a8e3c6a6fcb82`。没有把本轮新提交计入这 100 项。

逐项检查标题、变更文件、增删统计和提交职责；对疑似范围不符的提交进一步阅读 patch。区分业务/基础设施、测试、工具、文档和构建依赖。未按某个动词批量判错。

### 8.1 两项具体建议

| SHA | 原标题 | 建议标题 | 判断 |
| --- | --- | --- | --- |
| `2bde22feb33365df17d47690db3e3262f9b88648` | 统一媒体入口授权与 URL 解析 | 统一媒体入口信令授权并改用 Ada 解析 URL | 标题范围可以更具体，不是必须重写的错误 |
| `0a8f3f99d715049a3291264e7628991f8a486c16` | 统一推流与播放授权 | 统一一次性推流播放授权并加入推流设备管理 | 标题未直接反映新增设备管理，命名范围不足 |

第一项实际修改 60 个文件（1882/588 行），新增共享 signaling verify、HTTP target parser，把各媒体入口接入统一授权，并新增 Ada 子模块、移除 Boost.URL 依赖。依据包括 `.gitmodules`、`cmake/boost.cmake`、`media/http/http_target.cpp` 和 `media/http/signaling_verify.*`。Ada 迁移与统一 URL 解析有关，不能仅因文件多就判断职责无关。

第二项实际修改 19 个文件（1289/457 行），新增 `signaling/stream_token.*`、`push_device_store.go`、`push_http.go`，加入推流对账和结束授权失效，删除旧 `play_ticket.go` 及 WHEP 代理入口。属于业务和相应测试，设备管理与统一授权构成同一端到端需求。本轮没有证据要求把已发布提交重新拆开；未来同类改动可让设备 CRUD 与授权迁移的提交边界更易审核。

两个建议都用于提高历史检索和标题准确性，**不构成历史重写授权**。

### 8.2 保持原消息的代表项

- `c2e7f2700794`：记录取消错误与生命周期审查，少量符号命名也在同一审查语义内，不因“记录”强制改写。
- `440f762f9c70`：性能工具使用说明与 Pi 交接手册，虽然文档较长，仍是独立文档交付；“完善”对应具体文档对象。
- `4bd3b66689ef`：媒体源统一使用运行标识，跨多个模块保持同一身份事实，文件多不等于职责混杂。
- 其他 95 项未确认存在值得调整的标题与主要 diff 错位。

### 8.3 是否重写历史

**不建议，也未执行。** main 已发布，Pi 使用 `b4965b8`、`7788320` 等固定 SHA；单纯改善两个标题不足以抵消变更所有后继 SHA 和重定基线的成本。

没有选定重写起点或受影响范围。若未来用户另行批准，应先确定精确提交清单与所有引用方，保留旧 refs，再验证代码树完全一致。此处仅记录审核条件，不提供本轮 rebase/reword/amend/force-push 操作计划。

### 8.4 已审查的 100 项索引

分类根据实际路径；一个逻辑变化可以包含实现与其测试、文档，不强制每类一个提交。

| SHA | 标题 | 实际类型 | 文件数 |
| --- | --- | --- | ---: |
| `799455038c1d` | 明确RTSP播放V2调度结论适用范围 | 文档 | 1 |
| `1e83b923afbe` | 记录RTSP播放V2生命周期时序验证结论 | 文档 | 1 |
| `4690a16cc673` | 覆盖RTSP播放媒体失败后的迟到PLAY时序 | 测试 | 1 |
| `7788320af210` | 记录代码简化成果合并结果 | 文档 | 1 |
| `2d07fc49c7e2` | 记录代码简化续审结论与验证结果 | 文档 | 1 |
| `07702f07a3b8` | RTSP 播放关闭时只判断一次源是否存在 | 业务/基础设施 | 1 |
| `a5b3e4e3bf88` | 共享 PS 输出启动时不再判断源为空 | 业务/基础设施 | 1 |
| `64a562a469c5` | TCP 监听地址与端口改为启动参数 | 业务/基础设施 | 5 |
| `5da30ee990c4` | RTSP 拉流解析与连接后的终止条件合并为一次判断 | 业务/基础设施 | 1 |
| `30971c864996` | HLS 读取下一请求前不再重复检查 socket | 业务/基础设施 | 1 |
| `a3a1e282e6f6` | HTTP-FLV 媒体输出直接交给 enqueue 处理空数据 | 业务/基础设施 | 1 |
| `5ea776b4dd06` | 国标 UDP 发送 RTCP 定时只在调用方检查终态 | 业务/基础设施 | 1 |
| `8fbdf178db42` | RTSP 播放准备呈现时不再重置新建会话的状态 | 业务/基础设施 | 1 |
| `8d6982041ffe` | RTSP TCP 推流 RTCP 定时只在调用方检查终态 | 业务/基础设施 | 1 |
| `3f7b629386cb` | RTSP 连接读循环统一由消费字节检查结束 | 业务/基础设施 | 1 |
| `0303b0e081c9` | 取出国标发送会话时直接在锁内返回结果 | 业务/基础设施 | 1 |
| `7b1fce67138e` | TCP 写入 span 重载不再重复判空 | 业务/基础设施 | 1 |
| `1e3885e20b0b` | 精简TCP写回调重复判空 | 业务/基础设施 | 1 |
| `38364aa96fbb` | 审查全协议时序与生命周期精简空间 | 文档 | 1 |
| `c2e7f2700794` | 记录取消错误处理与生命周期审查结论 | 文档、测试 | 2 |
| `4abaa83d2c4b` | 精简国标UDP发送会话错误回调 | 业务/基础设施 | 1 |
| `80ba1276a09e` | 精简RTSP UDP发布会话错误回调 | 测试、业务/基础设施 | 2 |
| `9cd85d3d96f6` | 精简网络会话取消错误日志过滤 | 测试、业务/基础设施、构建/依赖 | 7 |
| `76355e435a76` | 审查性能测试方案与工具充分性 | 文档、测试 | 5 |
| `21bba8cf837e` | 完善性能工具身份核验与建立期保护 | 文档、工具、测试 | 8 |
| `440f762f9c70` | 完善性能工具使用说明与 Pi 交接手册 | 文档 | 17 |
| `2b1c2782f90a` | 完善诊断采样与运行包可追溯性 | 工具、测试、构建/依赖 | 4 |
| `7706ae2f28da` | 修正性能测量窗口与故障证据判定 | 工具、测试 | 6 |
| `83c44591bf15` | 审计性能工具能力与验收缺口 | 文档 | 1 |
| `b0b67da8a3f0` | 完善性能工具验证与非交互执行 | 工具、测试 | 25 |
| `c08f32a50c62` | 隔离并行测试的媒体端口资源 | 构建/依赖 | 1 |
| `9486c8602ef3` | 补齐混合协议负载与统一测量工具 | 工具、测试、构建/依赖 | 25 |
| `fb246637bcaf` | 补齐原生播放工具独立凭据与媒体健康检查 | 文档、工具、测试、构建/依赖 | 11 |
| `fcbc2a81fdb8` | 记录性能工具能力与阻塞项 | 文档 | 1 |
| `b4965b8a928e` | 强化真实 EMFILE 恢复测试 | 测试 | 1 |
| `a753d54cec3c` | 修复 RTSP 拉流迟到 PLAY 回调空指针 | 测试、业务/基础设施 | 2 |
| `e892e1d0e14f` | 补齐 WHEP 容量测试授权与持续进度观测 | 工具 | 1 |
| `3af94966ff67` | 阻止已关闭 RTSP 拉流被迟到解析重新建立 | 测试、业务/基础设施、构建/依赖 | 3 |
| `570f92168bb1` | 补充 TCP 会话提前关闭回归 | 测试、构建/依赖 | 2 |
| `468c98812c73` | 收口 RTSP 父子回调保活与逐层关闭 | 测试、业务/基础设施 | 6 |
| `6fd8ed7ee6b7` | 明确 WebRTC 传输回调的会话保活 | 业务/基础设施 | 2 |
| `a4b029f42c52` | 补充国标 TCP 建立与关闭交错回归 | 测试、构建/依赖 | 2 |
| `a915cbb60f52` | 修复 RTMP 启动与工作线程停止交错 | 测试、业务/基础设施、构建/依赖 | 3 |
| `d8a24286b933` | 收口国标父子回调保活与停机关闭入口 | 测试、业务/基础设施 | 4 |
| `f441a46bfcd9` | 验证工作线程停止时逐层关闭排空 | 测试 | 1 |
| `afd2cc3e3344` | 修复 RTMP 关闭窗口的迟到媒体输出 | 测试、业务/基础设施、构建/依赖 | 4 |
| `1a10076ac05b` | 补充国标 PS 拓扑边界测试 | 测试 | 1 |
| `94cad9ad86a7` | 简化 WHIP 可选音频初始化路径 | 业务/基础设施 | 1 |
| `b113b83eba56` | 简化国标媒体拓扑发现分支 | 业务/基础设施 | 1 |
| `da626276da63` | 简化 RTSP 收包与轨道解析控制流 | 测试、业务/基础设施 | 4 |
| `b3212fc0b023` | 修复 DTLS 失败后的迟到超时处理 | 测试、业务/基础设施、构建/依赖 | 3 |
| `a1f2f2034c04` | 明确 TCP 错误回调所有权移交 | 业务/基础设施 | 1 |
| `16ec8236b79f` | 精简 WHEP 会话关闭生命周期 | 测试、业务/基础设施、构建/依赖 | 4 |
| `72be98051842` | 移除国标 RTP 发送器冗余关闭状态 | 测试、业务/基础设施、构建/依赖 | 6 |
| `e4187c33c89f` | 精简 UDP 传输终止状态与错误传播 | 测试、业务/基础设施 | 3 |
| `2a863a408798` | 精简 TCP 传输终止状态与错误处理 | 测试、业务/基础设施 | 3 |
| `d7b4b020304f` | 精简 RTSP UDP 发布启动职责 | 测试、业务/基础设施、构建/依赖 | 6 |
| `1d60c93617a7` | 复用 RTSP ANNOUNCE 解析结果 | 测试、业务/基础设施、构建/依赖 | 7 |
| `7ca4a9b3166b` | 精简 RTSP 拉流配置解析 | 测试、业务/基础设施、构建/依赖 | 5 |
| `619e53e0b232` | 收口 RTSP 播放授权后初始化时序 | 业务/基础设施、构建/依赖 | 2 |
| `f0f309993bc6` | 收口 RTMP 播放授权后初始化时序 | 测试、业务/基础设施、构建/依赖 | 3 |
| `ee40b5d8987e` | 收口 HTTP-FLV 授权后初始化时序 | 测试、业务/基础设施、构建/依赖 | 5 |
| `87e18a06eccf` | 收口 WebRTC 一次性授权时序 | 测试、业务/基础设施、构建/依赖 | 6 |
| `af82c364138a` | 精简 HLS 播放列表等待 | 测试、业务/基础设施、构建/依赖 | 4 |
| `5023b2c24559` | 精简 WHEP 创建与内部启动职责 | 测试、业务/基础设施 | 5 |
| `3dc16031b8f3` | 精简 WHIP 创建与内部启动职责 | 测试、业务/基础设施 | 5 |
| `894c69cd9780` | 记录 HLS 修复与历史复杂度复核 | 文档 | 1 |
| `f1a6d49cd535` | 修复 HLS 授权前创建分片器 | 测试、业务/基础设施 | 4 |
| `5b8f992dd8e6` | 完善推流设备与播放地址管理 | 测试、业务/基础设施 | 6 |
| `549061b0f493` | 保持 WebRTC 建立超时约束 | 测试、业务/基础设施、构建/依赖 | 4 |
| `2bde22feb333` | 统一媒体入口授权与 URL 解析 | 工具、测试、业务/基础设施、构建/依赖 | 60 |
| `0a8f3f99d715` | 统一推流与播放授权 | 测试、业务/基础设施 | 19 |
| `4bd3b66689ef` | 媒体源统一使用运行标识 | 工具、测试、业务/基础设施 | 81 |
| `17c190473bea` | 统一会话关闭为投递到自身 io 执行清理 | 测试、业务/基础设施 | 16 |
| `87bc65c76f59` | PS 输出、HLS、监听与拉流启动失败交由上层清理 | 测试、业务/基础设施 | 10 |
| `02d6a50731ee` | WebRTC 会话启动失败交由 shutdown 统一清理 | 测试、业务/基础设施 | 5 |
| `b898f2a5d46e` | RTSP 媒体与连接失败交由 shutdown 统一清理 | 业务/基础设施 | 6 |
| `8e9432fd10dd` | RTMP 播放与推流失败交由 shutdown 统一清理 | 业务/基础设施、构建/依赖 | 4 |
| `6d10f0ece3bb` | GB28181 会话启动失败交由 shutdown 统一清理 | 测试、业务/基础设施 | 8 |
| `198f61ddb238` | 精简输入空闲计时实现 | 测试、业务/基础设施、构建/依赖 | 15 |
| `3034cb291a3d` | 删除空闲计时简化后残留的头文件包含与格式改动 | 业务/基础设施 | 3 |
| `323861df76f8` | 去掉 GB 接收测试中的帧计数辅助类 | 测试 | 1 |
| `caacd25d6542` | 输入空闲计时改为收到数据即刷新 | 测试、业务/基础设施 | 21 |
| `d7f145650ee6` | accept 失败后固定 3 秒重试 | 测试、业务/基础设施、构建/依赖 | 7 |
| `3e99476c405f` | 撤销接收会话关闭等待机制 | 测试、业务/基础设施、构建/依赖 | 31 |
| `874b4f59767f` | 区分关闭完成与停止并处理迟到完成 | 测试、业务/基础设施、构建/依赖 | 17 |
| `1219a985a8b6` | 删除接收会话在清理完成后才返回 | 测试、业务/基础设施、构建/依赖 | 27 |
| `74f0c6d22614` | 校验 PSM 结构边界并按不同 SID 计算容量 | 测试、构建/依赖 | 2 |
| `971a803e34af` | PSM 解析成功后才提交并通知空拓扑 | 测试、业务/基础设施、构建/依赖 | 5 |
| `b49a45be1797` | 按活动 PSM 条目识别 PS 拓扑变化 | 测试、业务/基础设施、构建/依赖 | 6 |
| `45bdfaea5f86` | 忽略只有封装头的空音频输入 | 测试、业务/基础设施 | 7 |
| `92fe3117edf9` | 拉流轨道未就绪时不发布也不刷新出帧期限 | 测试、业务/基础设施、构建/依赖 | 4 |
| `af5ee59a66bb` | 排除空载荷输入并明确输入空闲契约 | 测试、业务/基础设施 | 7 |
| `bf51e4f96509` | 统一按发布媒体帧刷新输入空闲期限 | 测试、业务/基础设施、构建/依赖 | 12 |
| `a8a486e3a184` | 收口 accept 错误分类与输入媒体空闲判定 | 测试、业务/基础设施、构建/依赖 | 20 |
| `67792f959cb9` | 放宽 GB28181 点播应答与目录状态的兼容性 | 测试、业务/基础设施 | 19 |
| `87a686bf2967` | HLS 跳过 Opus 轨道并支持纯音频切片 | 测试、业务/基础设施、构建/依赖 | 4 |
| `1e3e1f6ca2d8` | 为媒体服务器控制接口增加鉴权 | 测试、业务/基础设施、构建/依赖 | 9 |
| `f3b29662f3af` | GB28181 PS 中不支持的音频降级为纯视频 | 测试、业务/基础设施、构建/依赖 | 5 |
| `1d28c768cecc` | Opus 直通协商按 RFC 7587 处理缺省 maxaveragebitrate | 测试、业务/基础设施 | 2 |

## 9. 规范落地与边界

`docs/development_conventions.md` 基于主仓库已有本地规范整理，补全实际格式配置、路径长度计算、宏/C linkage 边界、二次格式化及提交验证要求。长期的 snake_case、guard、class 分组、自包含规则保留，但明确本轮没有授权改变这些源码结构。

旧本地文档把“修正/明确/精简/收口”等词作为模板限制；本轮按用户最新明确要求修正为依据实际 diff 判断，而非列禁词。中文、简洁、动宾、单一逻辑变化、不强制 Conventional Commits 或正文仍保留。

`docs/` 被本地 ignore 规则覆盖，本轮只对这两份授权文件精确 force add，没有改变 `.gitignore`，没有纳入 AGENTS、其他报告或大型产物。

本轮提交顺序：

1. `09213ec16c68b03084322ca0edce1b82a60ec5f4`：统一自有C++代码排版。
2. `5722ffb908f0e214d7d8898ca7e8ef6466a9aaa6`：统一自有头文件包含顺序。
3. 本报告所在提交：明确代码格式与提交消息规范。

未完成事项：无本轮必需项。严格的全部头文件独立编译检查、命名/class 布局整改和历史重写均不属于本轮。不能将没有发现排序例外解释为对所有未来依赖顺序的保证。

最终边界：生产行为不变，main 工作区未改，Pi 工作区未改，third 未改，历史未改写，未 push。
