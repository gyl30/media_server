# Phase 7 与多协议 Web 控制台整合验收

本轮只整合已审查分支并验证，未继续优化或开发功能。结论为 **CONDITIONAL GO**：在本轮覆盖的 DEV、Chrome、直连 UDP 和 HTTP WHEP 范围内，关键回归通过，未发现未修复的 P0/P1。Firefox 实际 H.264 播放受本机 codec 限制；CTest 并行执行还有 fixture 端口冲突，不能报告为并行通过。

## 代码基线与合并

| 项目 | 值 |
|---|---|
| 合并前 main / Phase 7 HEAD | `dbd0e589ad66ba44cfa599535064dbf9ef958dd9` |
| Web HEAD | `6ea91d28af7e59930cb93d2af436388dd7dde13a` |
| origin/main、共同祖先 | `cafee7e28f96c3d8d998bb0191734a643edb9571` |
| 合并提交、实际受测代码 HEAD | `8b3a4952ff9f9276fdb591c2f0114c7272426ae1` |
| 分支、工作区 | `main`，`/home/gyl/example/media_server` |
| 方法 | `git merge --no-ff feature/web-console -m "整合多协议接入与 Web 控制台"` |
| 冲突 | 无，ort 自动合并 |
| push | 未执行 |

所有结果均来自上述合并 HEAD 的新构建，不以分支历史验收替代本轮结果。本报告自身的提交不改变受测源码。

Phase 7 七个提交及 Web 二十七个提交全部保留为 main 的祖先。Phase 7 提交为 `4abe523`、`b18d69f`、`06fe0f0`、`23e77ca`、`309940a`、`66779b9`、`dbd0e58`；完整提交清单和父节点保存在本轮 `baseline.json`、`merge-check.json`。

两侧文件级交集为 `CMakeLists.txt` 和 `bench/clients/webrtc_client.cpp`。合并后检查确认：

- CMake 保留 Web 的十种 `pull_decode` 测试模式及 Phase 7 的 `webrtc_client_lifecycle`；最终实际注册 72 项。
- WebRTC 客户端使用正确的 client/server key material 和 `dtls_role::client`；保留 Phase 7 的回调局部副本，以及 `close()` 清空 packet/error callback 的处理。
- GB UDP sender 的 vector 所有权移交、HTTP-FLV completion 驱动发送及其回归文件与 Phase 7 HEAD 相同。
- 没有人工冲突补丁，没有额外生产或测试代码修复，没有调整详细协议日志。

## 环境、命令与证据

本轮为 DEV 本地隔离回归，没有访问 TEST 性能环境，没有运行容量、A/B 或 profiling。编译器为显式 `/usr/bin/gcc-16`、`/usr/bin/g++-16`，版本 `16.0.1 20260315 (experimental)`；内核 `6.8.0-136-generic`。使用现有 Boost 1.92 静态库、`/home/gyl/ffmpeg901` SDK、`/home/gyl/bin/ffmpeg`、仓库 libSRTP 和 OpenSSL。

受限证据根目录：

```text
/var/tmp/media_server_final_integration_6ea91d2_7f_530u4
```

下文以 `$R` 表示此目录。根目录权限为 0700，关键日志和 JSON 为 0600；包含一次性 URL 的原始资料不提交到 Git。`SHA256SUMS` 记录关键日志、断言、脚本和结构化结果，`fixtures-sha256.json` 记录复用素材的 SHA256。

```bash
PKG_CONFIG_PATH=/home/gyl/ffmpeg901/lib/pkgconfig cmake -S . \
  -B "$R/build-normal" \
  -DCMAKE_C_COMPILER=/usr/bin/gcc-16 \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++-16 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBoost_DIR=/usr/local/lib/cmake/Boost-1.92.0 \
  -DBoost_USE_STATIC_LIBS=ON -DBUILD_TESTING=ON \
  -DMEDIA_SERVER_BUILD_BENCHMARKS=ON \
  -DMEDIA_SERVER_TEST_FFMPEG=/home/gyl/bin/ffmpeg \
  -DMEDIA_SERVER_SANITIZER=none
cmake --build "$R/build-normal" -j2
TMPDIR="$R/tmp" unshare -Urn sh -c \
  "ip link set lo up && ctest --test-dir '$R/build-normal' --output-on-failure"

cd signaling
GOTMPDIR="$R/go-tmp" go build -o "$R/signaling" ./
go test -count=1 ./...
go vet ./...
go test -race -count=1 ./...
```

缓存确认 FFmpeg 测试路径为上述程序。C++ 使用新目录；Go 输出在工作树外，未覆盖主工作区 build 或 Web 工作区二进制。

| 验证 | 本轮结果 | 证据，相对 `$R` |
|---|---|---|
| 普通 CMake / 完整 C++ 与测试目标 build | PASS | `logs/configure-normal.log`、`logs/build-normal.log` |
| 完整串行 CTest | PASS **72/72**，470.90 秒 | `logs/ctest-normal-serial-full.log` |
| 额外完整并行 CTest `-j2` | FAIL **69/72**，fixture 端口冲突 | `logs/ctest-normal-full.log` |
| Go build | PASS | `logs/go-build.log` |
| Go test | PASS，signaling 与 simulator 两包 | `logs/go-test.log` |
| Go vet | PASS | `logs/go-vet.log` |
| Go race | PASS，signaling 与 simulator 两包 | `logs/go-race.log` |
| ASan+UBSan 定向 CTest，开启 LSan | PASS **25/25**，196.11 秒 | `logs/ctest-asan-ubsan-directed.log` |

并行失败为 `pull_decode_whep_failures`、`pull_decode_whep_consent`、`pull_decode_whep_mixed_candidates`。原日志明确出现 `Address already in use`；这些 fixture 在同一 network namespace 并行使用本地 UDP 端口池。未删除测试、放宽断言或修改生产代码；改用串行完整执行后全部通过。此限制仍保留，不能声称已修复并行测试隔离。

## 生命周期及交叉路径

普通全量与 sanitizer 定向覆盖以下实际注册测试：

- `udp_transport`、`gb_sender_lifecycle`：datagram/endpoint、在途 buffer、错误与关闭、UDP/TCP sender、RTCP 和端口回收。
- `http_flv_auth`：鉴权、多个 viewer、背压、pending 发送关闭、异常断开和媒体结束。
- `webrtc_client_lifecycle`：error callback 内关闭、重复关闭、取消及弱所有权释放。
- `media_lifecycle`、`worker_sink_fanout`：源代际、跨 worker 结束、弱 sink 和 RTSP 公共播放起点。
- `rtmp_pull_lifecycle`、`rtsp_pull_shutdown`、`rtsp_pull_config`、六项 `tcp_transport_*`。
- 十项 `pull_decode_*`：RTMP/RTSP/WHEP 的真实拉流与媒体，WHEP 失败、worker shutdown、transaction、consent 和候选地址边界。

`media_lifecycle` 保留 RTSP 起点前迟到音频裁剪断言、起点后音频、连续 RTP 序号/timestamp 以及 RTCP SR 时钟对应关系；普通与 sanitizer 均通过。未放宽 PTS 断言。

另外复用 native WebRTC 客户端，执行一次性真实 RTP 回调内发送 RTCP、两次 `close()`、资源 DELETE、弱引用销毁和本地 UDP 端口重新绑定。普通及 ASan+UBSan 两次均为 `rtp=1 rtcp_send=1 errors=0 released=1 removed=1`。探针仅在受限临时目录，不新增仓库测试框架。证据为 `real-final/packet-close.log`、`native-close-asan/client.log`、`native-close-asan/result.json`。

## 真实七输入与双轨解码

复用 Web 验收的固定 H.264/AAC、H.264/Opus、GB H.264/PCMA 素材与 Go signaling/simulator，创建本轮自己的数据库、凭据、进程和端口。素材为既有合成 fixture；实际网络、协议栈、解码器和生产 session 均参与执行。

七输入分别通过真实 Chrome WHEP H.264/Opus 解码和 FFmpeg RTSP 双轨解码。两类证据边界不同：Chrome 检查 WHEP 视频 frames、音频 samples/duration/energy 增长；FFmpeg 通过 RTSP 输出独立检查视频/音频 PTS 严格递增和 demux/decode 错误。没有声称 FFmpeg 直接解码 WHEP。

| 输入 | WHEP 实际播放 | FFmpeg 严格双轨解码 | 3 秒视频帧 / 音频样本 |
|---|---|---|---|
| RTMP publish | PASS | PASS | 76 / 144384 |
| RTSP publish | PASS | PASS | 76 / 143360 |
| WHIP publish | PASS | PASS | 75 / 142336 |
| RTSP pull | PASS | PASS | 76 / 141312 |
| RTMP pull | PASS | PASS | 76 / 144384 |
| WHEP pull | PASS | PASS | 77 / 145408 |
| GB28181 | PASS | PASS | 76 / 24000 |

证据：`real-final/browser-multi_input/result.log` **102/102**，以及 `real-final/ffmpeg-seven-pts-green/seven-result.json` **7/7**。视频均为 H.264，音频实际解码，PTS 严格递增，没有 demux/decode 错误。WHEP 范围仍为已支持的 HTTP、直连 UDP、ICE-Lite 上游；未扩展 HTTPS、TURN、Trickle 或任意公网 full-ICE。

代表性交叉输出通过真实 HTTP-FLV FFmpeg 双轨解码：

| 路径 | 结果 | 视频帧 / 音频样本 |
|---|---|---|
| RTMP input → HTTP-FLV | PASS | 75 / 141312 |
| WHEP pull → HTTP-FLV | PASS | 76 / 143360 |
| GB input → HTTP-FLV | PASS | 76 / 24320 |

对应 `real-final/cross-media/*-result.json`。GB input → RTSP 已由七输入 FFmpeg 测试覆盖。

GB UDP 输出另复用 Phase 7 归档的既有 RTCP 探针，使用本轮 ASan+UBSan DUT，在独立 network namespace 内执行：RTMP → GB UDP sender → 透明 RTP/RTCP 转发 → GB receiver → HTTP-FLV 双轨解码。使用现有一次性授权 stub 与 Bearer 控制认证，不关闭生产鉴权；此探针不冒充 Go 的完整业务身份测试。

33 秒观察窗口收到 **5199** 个连续 RTP 包，七个进展窗口均有数据，**2234** 个不同 timestamp，SSRC/PT/endpoint 和 compound RTCP 验证通过。收到 **1 SR / 8 RR**；首次 SR 为 **25.49 秒**，实际覆盖既有 25 秒 RTCP 定时器。HTTP-FLV 解码 **75** 视频帧；receiver/sender/source 删除、服务退出和清理均通过。证据为 `gb-rtcp-integration/result.json` 与该目录日志。

## Web、共享上游与持久化

Chrome 十二组有效场景在同一合并 HEAD 上合计 **357/357 PASS**。`detail_ops` 在新建、初始无 GB live 的独立环境执行；其他十一组使用另一套同版本环境，破坏性的 `video` 放最后。不是将整个编排脚本的 FAIL 退出码改写为 PASS；逐组退出码、原始失败与有效结果映射见 `chrome-valid-aggregate.json`。

| Suite | PASS |
|---|---|
| detail_ops | 22/22 |
| multi_input | 102/102 |
| playback / playback_pull | 22/22、7/7 |
| devices / system | 75/75、19/19 |
| clipboard / mobile / responsive | 4/4、9/9、32/32 |
| video_entry / channels_failure / video | 8/8、9/9、48/48 |

覆盖七类添加、主动源自动拉流、被动 URL 确认/五分钟有效/一次消费、详情播放、单/四画面、快速替换、连接中切布局、路由跳转和 GB 失败不循环。`channels_failure` 是明确的接口故障注入，不作为实际媒体成功证据；suite 显式派发的 `pagehide` 不代替真实关页。

独立 GB 三 viewer 验证 **64/64 PASS**：独立 native H.264/音频解码和 WHEP resource、共享同一上游，已有 live 时新增 viewer 的 **INVITE 增量 0**；关闭 viewer 不停止共享 live，明确 stop 后结束旧代，重播进入新代。证据为 `real-final/gb-three-viewers-20261014-v2/result.json`。

数据库重启 **75/75 PASS**：仅停止自己的 DUT Go，保持媒体和 upstream；确认旧 pull/push/GB receiver 清除，SQLite 中仅精确替换三个本轮过期上游 URL 为独立新凭据，再以同一 Go ELF 和数据库启动。三个 source_id 保持、generation 更新，stopped 配置不启动，已消费地址仍拒绝重用，未确认 preview 不持久化，原 pending URL 保留原到期时间并实际首次推流成功。上游凭据刷新是 fixture 准备，不是新增产品自动刷新能力。

重启后四目标（RTSP/RTMP/WHEP pull、原 pending URL publisher）全部通过独立 FFmpeg 和 Chrome 双轨解码 **4/4**，使用 **8** 份独立播放凭据，资源 DELETE 完成。证据为 `restart-env/restart-v2-20261014/`、`restart-env/after-restart-decode-20261014-v2/result.json`。

真实 Chrome 标签页关闭 **78/78 PASS**：`page.close(run_before_unload=True)` 产生可信 `pagehide`；外部透明 HTTP 观察器收到并转发恰好一次 DELETE，核对原始请求与 SDP 请求/响应的 byte SHA256，观察到对应服务端 WHEP shutdown，发布源仍 publishing/media_ready。没有伪造 HTTP 状态或合成生命周期事件。证据为 `real-tab-close-observed-v2/evidence/`，其清理结果为 PASS，观察器线程和 socket 全部结束。

Firefox 本轮实际 **7/8**：页面与控制台检查通过，真实 WHEP 播放 **BLOCKED**。原生 video capabilities 提供 VP8/VP9/AV1，没有 H.264；未安装 codec、改变媒体行为或绕过断言。`restart-env/browser-firefox/` 保留原结果、截图和本轮失败 profile。

## Sanitizer 边界

独立 Debug build 使用只读复用的 ASan Boost 1.92 前缀：

```text
/var/tmp/media_server_phase7_asan_23e77ca_zFCjAKc8/boost-1.92-asan
```

`MEDIA_SERVER_SANITIZER=address` 实际启用 address+undefined；Boost.Context 使用 UContext/ASan，guard 校验通过。开启：

```text
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=1
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
LD_LIBRARY_PATH=$R/sanitizer-runtime
```

测试命令为新 build 下串行 CTest：

```bash
TMPDIR="$R/tmp" unshare -Urn sh -c \
  "ip link set lo up && ctest --test-dir '$R/build-asan' --output-on-failure \
  -R '^(udp_transport|gb_sender_lifecycle|http_flv_auth|webrtc_client_lifecycle|media_lifecycle|worker_sink_fanout|rtmp_pull_lifecycle|rtsp_pull_shutdown|rtsp_pull_config|tcp_transport_.*|pull_decode_.*)$'"
```

实际 **25/25 PASS**，并额外运行上述 GB RTP/RTCP 交叉探针和真实 RTP callback-close 探针。`sanitizer-final-scan.json` 没有 AddressSanitizer、LeakSanitizer 或 UBSan fatal report；保留 **12** 条 makecontext/swapcontext 兼容警告。测试依赖的 FFmpeg/OpenSSL 等共享库未全部重新插桩。

没有运行整合后的完整 sanitizer 72 项，也没有以历史 Phase 7 61/61 或 Web 原始 65/67 代替本轮结果。没有重新运行大规模性能测试。

## 原始失败与处理

所有失败均保留，没有人为破坏生产代码制造 RED，也没有通过修改生产或测试断言获得 GREEN：

1. 并行 CTest 的三个 WHEP fixture 端口冲突；串行完整 72/72 通过，隔离问题仍存在。
2. 初次短命启动脚本退出后 fixture 进程消失，UI 无法连接；改由持续存活的本轮父进程持有服务，保存 PID/starttick/exe/netns 清单。
3. `video` 按设计停止 RTMP publisher，之后再解码得到 6/7；将七输入解码放到破坏性操作之前后为 7/7。
4. 已启动 GB live 的 fixture 违反 `detail_ops` 初始无 live 前提；新建独立初始环境为 22/22，未改 suite。
5. 额外一次性 GB 桥接草稿误读 receiver/create 的返回字段 `rtcp_port`，导致 KeyError；草稿未建立 sender，不作为生产缺陷。三条已完成 HTTP-FLV 解码仍保留；GB 输出改用既有、完整的 Phase 7 探针验证，不增加授权旁路。
6. 首次独立 HTTP 观察器在此前使用过的 48000 端口 bind 失败；保留失败与 cleanup FAIL。第二次选择新且已核对可 bind 的 48400 系列独立端口，断言保持，关页 78/78 和清理通过。没有修改系统配置或生产监听器。
7. 临时 native probe 的首次编译及过期 fixture 请求失败保留；当前 main 库上的最终普通与 sanitizer 实际收包关闭均通过。
8. Firefox H.264 codec 限制仍为 BLOCKED。

前三轮一次性编排的整体 FAIL 退出码仍存在于原始证据。最终结论依据有退出码和断言映射的有效测试，不根据日志中出现 PASS 字样判定整个脚本成功。

## 清理与最终边界

`final-resource-check.json` 确认本轮各 fixture manifest 中没有原 PID/starttick 身份残留，没有本轮 runtime 进程和固定监听/UDP 绑定。真实关页观察器、Chrome、Firefox、Go、media_server、publisher、simulator 均已结束；隔离 namespace 随所属进程退出释放。

仅将本轮 Firefox 失败 profile 从用户 cache 移到受限证据目录。新构建、临时验证脚本、受限日志与截图保留在 `$R` 供审核，不加入 Git；未删除其他 Agent 的文件或工作区。

Git 最终检查确认：

- main 仅增加正常 merge 和本报告提交；没有改写任何既有历史，没有 push。
- Web 工作区 HEAD 仍为 `6ea91d2`，跟踪/未跟踪状态未变化；其忽略的历史脚本只读。
- 设计原型全部 **120** 个文件 SHA256 与合并前清单一致，保持 untracked，未移动、删除、暂存或修改。
- `third/ada`、`third/libsrtp` gitlink 和实际 submodule 状态与合并前相同。
- 合并没有新增 source registry、播放器状态、token 消费事实、终态标志、缓存层、反向控制或延迟关闭；bounded queue 与 owner worker 边界未调整。
- 除本报告外，合并后的生产、测试、性能工具和配置没有额外修改。

**验收：CONDITIONAL GO。** 支持范围内的 Chrome/真实七输入、生命周期、共享 live、重启、串行完整 CTest 和定向内存安全验证通过；Firefox H.264 播放与 CTest 并行 fixture 隔离保持明确限制。没有本轮确认但未修复的 P0/P1。后续推送由用户审核后决定，本轮到此停止。
