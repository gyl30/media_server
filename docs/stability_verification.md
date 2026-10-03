# 稳定性验证记录

日期：2026-10-02。起点 `728178f6d09587110544faf0c04dd1f3e4bab669`，生产修复基线 `5c2cbd4e03f6773cedf45a44e1fb498bf7a876bb`。验证期间没有恢复废弃接口，也没有改变媒体核心架构。

## 构建与环境

Ubuntu 22.04，Linux 6.8.0-136-generic；i7-13700KF，1 socket、16 cores、24 logical CPUs；RAM 31 GiB；GCC/G++ 16.0.1（20260315 experimental），CMake 3.31.10，Boost 1.92.0，jemalloc 5.3.0_0，OpenSSL 3.0.2，libSRTP 2.7.0。

服务静态链接 `/home/gyl/ffmpeg901` 的 FFmpeg 9.0.1 库：avcodec 63.1.101、avutil 61.1.101、swresample 7.1.101、swscale 10.1.101。测试 CLI 是 `/home/gyl/bin/ffmpeg` 7.1.1，不能将两个版本混写。Sanitizer 构建禁用 jemalloc。

所有 configure/build 使用以下 pkg-config 路径，避免误选系统 libSRTP 2.4.2：

```bash
export PKG_CONFIG_PATH=/tmp/libsrtp-2.7-prefix/lib/pkgconfig:/home/gyl/ffmpeg901/lib/pkgconfig
cmake -S . -B build_verify -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DMEDIA_SERVER_BUILD_BENCHMARKS=ON
cmake --build build_verify -j12
ctest --test-dir build_verify --output-on-failure
```

本次独立构建位于 `/tmp/media_server_verify-728178f/build_{verify,asan,ubsan,tsan,perf}`；另对仓库 `build` 执行完整构建及 CTest。普通、独立 RelWithDebInfo、ASan+UBSan、独立 UBSan、TSan 构建均成功，CTest 均为 **7/7 PASS**。

七个注册测试覆盖服务帮助、四种非法配置、媒体生命周期，以及独立 FFmpeg RTMP 解码。非法配置要求退出码 1、usage 正确、stderr 为空，不能用 WILL_FAIL 掩盖 Sanitizer 崩溃。

## Fixture 与复现

```bash
/home/gyl/bin/ffmpeg -hide_banner -loglevel error \
  -f lavfi -i testsrc2=size=1280x720:rate=30 \
  -f lavfi -i sine=frequency=1000:sample_rate=44100 -t 10 \
  -c:v libx264 -preset ultrafast -tune zerolatency -g 30 -pix_fmt yuv420p \
  -c:a aac -b:a 96k -ac 2 -f flv h264-aac.flv
```

Fixture：H.264 1280×720@30 + AAC 44.1 kHz stereo，10.023 秒，9,796,818 bytes，约 7.819 Mbit/s；SHA-256 `446b462c4b408d997c17cd4bab82d3ca4e9bcda1f04c758471043518d62b800f`。重复运行必须使用相同文件，编码器版本不同可能改变 hash 和码率。

```bash
python3 -m pip install -r bench/requirements.txt
python3 bench/lifecycle_verify.py smoke --build-dir build_verify \
  --fixture h264-aac.flv --port-base 21930 --output verify/smoke
python3 bench/lifecycle_verify.py replacement --build-dir build_verify \
  --fixture h264-aac.flv --generations 20 --output verify/replacement
python3 bench/lifecycle_verify.py replacement --build-dir build_verify \
  --fixture h264-aac.flv --inputs gb --gb-transport tcp_sender_active \
  --generations 20 --output verify/replacement-gb-tcp
python3 bench/lifecycle_verify.py churn --build-dir build_verify \
  --fixture h264-aac.flv --iterations 300 --batch 20 --output verify/churn
python3 bench/lifecycle_verify.py disconnect --build-dir build_verify \
  --fixture h264-aac.flv --output verify/disconnect
python3 bench/lifecycle_verify.py soak --build-dir build_verify \
  --fixture h264-aac.flv --duration 600 --output verify/soak
```

脚本复用已有 RTMP/RTSP/WHEP/HLS 客户端、WHIP publisher 和 GB 网络 pair，不实现新的生产协议栈。HTTP-FLV 用 aiohttp 处理 chunked framing，再确认实际媒体 tag；RTMP 首媒体计量排除 codec configuration。每个运行保存 `commands.json`、子进程日志和 `result.json`。并行运行应设置不同 `--port-base`；性能测试串行运行。

## 真实协议与生命周期

RTMP/RTSP 使用真实 H.264+AAC fixture 发布。WHIP 工具发送 H.264 测试包和有效 Opus 静音，接收端生成 H.264+AAC source；WHEP 输出音频是共享 AAC→Opus。WHIP 的合成视频只验证协议、轨道和媒体包前进，不作为真实画面可解码证明。

每种 RTMP、RTSP、WHIP 输入同时运行 RTMP、RTSP、HTTP-FLV、HLS、WHEP 输出，各 1 viewer 和各 4 viewers。GB UDP、TCP sender active、TCP sender passive 经真实网络输出再接收为 relay source，验证 RTMP、RTSP、HTTP-FLV、WHEP 的音视频前进。独立 FFmpeg 对 RTMP/RTSP 输入后的 RTMP、RTSP、HTTP-FLV、HLS 输出实际解码双轨。

换源覆盖 RTMP、RTSP、WHIP、GB UDP 各 20 代，同名 source；另测 GB TCP 20 代。旧 HTTP viewer 收到 EOF，旧 HLS playlist 结束或 404，新 generation 的 HLS session Location 不复用。公共媒体测试另验证跨 worker 的 400 帧顺序和结束、旧 registry pointer 不删除新 source、共享 PS/AAC egress 只在同 generation 内复用，以及旧派生对象 weak reference 释放。

Churn 为 RTMP、RTSP、HTTP-FLV、HLS、WHEP 各 300 viewers，WHIP create/media/delete 300 sessions，GB create/media/delete 300 pairs；每批 20。HLS 300 个实际 session URL 在既有 inactivity lease 后均要求 `403 invalid hls session`。关闭后检查 FD 和线程数回到基线，正常 DELETE 后重复 DELETE 必须 404。媒体公共测试另执行 300 次 sink churn。

异常断开覆盖：已流动媒体的 RTMP/RTSP publisher 和 viewer TCP RST、HTTP response 中断、RTMP 半握手 RST、RTSP SETUP 不 PLAY、GB UDP peer 消失、WHEP/WHIP ICE+DTLS 建立后直接断开，以及只转发 STUN、丢弃 DTLS 的握手中断。测试在现有 15 秒 establishment / 30 秒 activity timeout 内检查 WebRTC 资源自然删除、一次 shutdown、端口可重新绑定；不提前 DELETE 制造成功结果。GB UDP 没有自动 peer timeout：先确认停止出媒体，再显式删除 receiver，验证 EOF、404、RTP/RTCP 端口释放。最后验证服务仍能正常播放、FD/线程数收敛。

## Sanitizer

ASan 构建使用 `/tmp/boost_1_92_asan_static`，Boost.Context 实际为 ucontext 且包含 ASan 支持；项目 CMake 编译链接 guard 原样通过，没有绕过。

```bash
cmake -S . -B build_asan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DMEDIA_SERVER_BUILD_BENCHMARKS=ON -DMEDIA_SERVER_SANITIZER=address \
  -DBoost_DIR=/tmp/boost_1_92_asan_static/lib/cmake/Boost-1.92.0 \
  -DBoost_USE_STATIC_LIBS=ON
cmake --build build_asan -j12
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir build_asan --output-on-failure
```

协议套件使用 Sanitizer server 和普通验证客户端：`--build-dir build_asan --client-dir build_verify`。独立 UBSan 改用 `-DMEDIA_SERVER_SANITIZER=undefined`，普通 Boost；设置 `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`。所有 child 日志均检查 Address/Leak/Undefined/Thread Sanitizer 报告，服务正常退出。ASan 关于 makecontext/swapcontext 的标准 warning 单独保留，不隐藏。

TSan 的 configure、完整 build、7/7 CTest、真实多协议 smoke 和 4×20 换源均可执行，无 TSan 报告。但当前 thread 配置链接普通 Boost.Context fcontext：编译没有 `BOOST_USE_TSAN/BOOST_USE_UCONTEXT`，库仅有 jump/make/ontop_fcontext，没有 TSan fiber hooks。不能据此声称 fiber 切换下的竞态检测可靠或所有并发均已证明无竞争；预编译 FFmpeg/libSRTP 也未插桩。本阶段按环境限制记录该边界，没有为 TSan 修改生产架构。

## 十分钟 soak

修复后实跑 **614.571 秒、21 个 source generations**。每代 4 viewers/protocol，RTMP、RTSP、WHEP、HLS 和 HTTP-FLV 同时播放。

| 指标 | 结果 |
| --- | --- |
| RSS 初始 / 最终 / 最大 / median | 21,792 / 66,148 / 81,080 / 66,298 KiB |
| VmSize 初始 / 最终 / 最大 | 195,972 / 478,596 / 478,596 KiB |
| FD 范围 | 27–47 |
| 线程 | 始终 6 |
| 平均服务 CPU | 0.0504 cores |
| RSS 四个时间段 median | 63,988 / 66,228 / 67,772 / 66,028 KiB |

观察到分配器/栈缓存建立后的内存高水位，末段回落；本次时长内没有持续线性上涨，FD/线程没有累积增长。该结果不等于任意长时间和任意负载下的无泄漏证明。ASan churn 的 RSS 还受 quarantine/fiber stack cache 影响，应与退出时 leak 检查及普通构建趋势一起判断。

修复后的完整 churn 资源：普通 RSS 最终 70,616 KiB、viewer wave 最大 87,680 KiB；UBSan 最终 198,472 KiB，GB 阶段基本保持不变；ASan 最终 16,929,316 KiB，阶段中明显增长。三者 FD 均 26→26、线程均 6→6，HLS 300 个 session 均过期，服务正常退出且无 Sanitizer/leak 报告。

对 ASan 高 RSS 做了受控 GB 对照（2 批×20 pairs）：默认选项 RSS 137,188→1,272,264→2,124,036 KiB；诊断性关闭 `detect_stack_use_after_return` 后 135,292→377,512→371,088 KiB；仅关闭 quarantine 后 136,844→1,125,308→2,024,680 KiB。该对照支持 fake stack 是主要增长来源，不能把默认 ASan RSS 直接外推为普通构建泄漏。关闭 fake stack/quarantine 的运行仅用于定位，正式 300 轮 ASan 验收保留原始检测选项。原始结果为 `asan-gb-memory-comparison.json`。

## 本轮发现并修复的缺陷

独立 FFmpeg RTMP 播放 H.264+AAC 报 `No start code is found`、无视频尺寸、缺失 AAC；同源 HTTP-FLV 可解码，仓库 native RTMP 计数客户端却能够通过。CLI 7.1.1 和系统 CLI 4 均复现。

根因：`rtmp_session::on_play` 回调中的 `play->startup()` 立即发送 codec configuration，ireader 依赖直到回调返回后才发送 Play.Start；FFmpeg 丢弃提前收到的初始化数据。

最小修复：在 muxer startup 前调用现有 `rtmp_server_start`，回调返回 `RTMP_SERVER_ASYNC_START`，避免依赖再次发送启动响应。没有恢复旧 defer/state/reader。新增公开边界 `rtmp_decode` CTest 在修复前稳定 RED（1.42 秒），修复后普通、RelWithDebInfo、ASan、UBSan、TSan 均 GREEN；提交 `5c2cbd4e03f6773cedf45a44e1fb498bf7a876bb` 独立提交并 push。

测试工具修正单独记录：RTSP CLI 使用 `-timeout`；HTTP-FLV 解 chunking 后确认媒体；RTMP 排除初始化数据计量；WHEP 首媒体要求视频+Opus；检查 SRTP 失败、UDP drop/queue full、实际窗口和资源回收。新增 HLS 过期断言曾误用 404，核实接口后改为严格检查 `403 invalid hls session`，重新执行完整 churn；原失败日志保留。它们不是新增的生产缺陷。

## 高风险边界及证据范围

- 跨 worker sink 投递与停止：公共测试验证有序媒体和结束，真实 fanout/换源/断开覆盖生产调用边界。
- 同名 source replacement：旧 owner 移除不会删除新 source；旧派生输出不与新 generation 共用，旧 viewer 和 HLS session 收口。
- 共享 PS 输出和 AAC→Opus egress：同源复用、跨代隔离、实际 AAC 音频转码及 SRTP 双轨前进。
- session owns shutdown：重复删除 404、WebRTC 一次 shutdown、异常后的恢复播放、FD/端口释放。
- HLS retention/session lease：实际媒体分片前进、换代结束、300 session 过期；不新增测试专用 timeout。
- 异常输入与内存安全：合法协议握手被中断、RST、SETUP 不 PLAY、Sanitizer 全量 CTest 和真实协议运行；没有对第三方目标扫描。

这些是明确执行过的边界，没有声称穷尽所有网络输入、codec 或未来 workload。

## 容量、性能与最终提交

WHEP 100 / 250 / 500 / 750 / 1000 viewers 五级全部通过；最高级服务 CPU 3.7600 cores、RSS median 360,404 KiB、峰值 394,440 KiB、6 threads、FD median 1027、8.0135 Gbit/s、首媒体 p50 510 ms，UDP drop/queue full/各类失败为 0，全部资源正常删除。达到本轮 1000 上限后停止；没有宣称已经找到最大容量。

| 套件 | 普通 | ASan+UBSan | 独立 UBSan | TSan（有上述限制） |
| --- | --- | --- | --- | --- |
| 全量 CTest | 7/7 PASS | 7/7 PASS | 7/7 PASS | 7/7 PASS |
| 真实多协议 smoke | PASS | PASS | PASS | PASS |
| RTMP/RTSP/HTTP-FLV/HLS 独立 FFmpeg 解码 | PASS | PASS | PASS | RTMP/RTSP/HTTP-FLV PASS |
| 四类输入各 20 代换源 | PASS | PASS | PASS | PASS |
| 七类各 300 churn + HLS 300 过期 | PASS | PASS | PASS | 未运行 |
| 严格异常断开 12 cases | PASS | PASS | PASS | 未运行 |
| GB TCP 20 代换源 | PASS | 未运行 | 未运行 | 未运行 |
| 614 秒多协议 soak | PASS | 未运行 | 未运行 | 未运行 |

实际最终套件为 `final3-smoke-{normal,asan,ubsan}`、`final2-smoke-tsan`、`final2-replacement-{normal,asan,tsan}`、`final-replacement-ubsan`、`final-replacement-gb-tcp`、`final4-churn-{normal,asan,ubsan}`、`final4-disconnect-{normal,asan}`、`final-disconnect-ubsan`、`final-soak`。较早的修复前结果和测试工具失败日志不能替代这些最终验证。

三轮性能已完成：独立 `build_perf`，18 workloads、54 次运行，每次 warmup 10 秒、measurement 30 秒，失败计数均为 0。WHEP 1000 viewers 的三轮 median 为 CPU 3.7602 cores、RSS 379,504 KiB、8.0275 Gbit/s、双轨首媒体 p50 502 ms。全部工作负载的 CPU/RSS/吞吐/延迟与历史比较见 [performance_baseline.md](performance_baseline.md)。

可审阅的资源采样与阶段汇总：[stability.json](verification_results/stability.json)、[whep_capacity.json](verification_results/whep_capacity.json)、[performance_baseline.json](verification_results/performance_baseline.json)。原始证据目录：`/tmp/media_server_verify-728178f`，含 RED/GREEN 日志、每条命令、协议结果、资源采样和 benchmark 单次结果。所有本次启动的 workload 子进程均已结束。

本轮独立提交：

- `33f168287ac545f24c4d761ad05c5e1642377758` 恢复服务命令行边界的 CTest 验证。
- `59cc4e8c918bf6e5f0c6ab51bb7de5a32bb27d14` 补充媒体换代与派生资源生命周期回归测试。
- `5c2cbd4e03f6773cedf45a44e1fb498bf7a876bb` 修复 RTMP 播放初始化数据先于控制响应。
- `8af98842f69d3e63a7e2d95b29f454d606992412` 补充真实协议生命周期验证与性能计量工具。
- 本文件所属提交：记录稳定性验证与三轮性能基线，仅包含报告和结果 JSON。

最终交付时核对 `git status --porcelain=v1` 为空，`git fetch origin` 后本地 HEAD 与实际 `origin/main` 一致；FINAL_HEAD 为本文件所属的最终报告提交，完整 SHA 见交付答复，避免在文件内自引用提交 hash。

建议下一阶段：保留当前代码和 fixture 为新基线；在具备 TSan fiber hooks 的独立依赖环境补可靠竞态验证，并按实际业务码率选择更长时段的运行观测。上述建议不属于本轮继续实施范围。

## 产品互操作时间戳检查：日志交错边界

在 `2a70363` 上使用 FFmpeg 生成的连续 90 秒 H264+AAC 素材，RTSP TCP 输入、HTTP-FLV 解码 40 秒：旧 `codec_interop_verify.py` 误报 4 次视频 PTS 回退。原始日志显示 `showinfo` 的颜色/校验和续行与 `ashowinfo` 交错，宽泛匹配把音频 PTS 归入视频；不是这条媒体链路的时间戳错误。

解析器现在只接受对应 filter 前缀后紧跟 `n / pts / pts_time` 的完整帧记录。用实际新表达式重读两份 HTTP-FLV 失败日志后，音视频回退均为 0；重跑 40 秒解码输出 1,200 帧、音视频持续推进、回退均为 0，全量 CTest 10/10 PASS。破碎日志不作为完整帧记录，因此日志计数不是精确解码帧数；后者仍取 FFmpeg progress。

同一修正仍保留 RTSP 播放的真实音频回退：RTSP UDP 输入的 60 秒记录有 8 次，连续素材 RTMP 输入的 40 秒记录有 2 次。这些结果尚未通过，不能将解析修正当成媒体问题已解决。原始 RED/GREEN、抓包、命令与构建结果保存在 `/tmp/media_server_product_current-2a70363-dD0La6`。

## RTSP Sender Report 的媒体时钟映射

随后独立复现确认：连续 90 秒 FFmpeg 生成素材、RTMP 输入、RTSP TCP 播放 40 秒仍有 2 次音频 PTS 回退，与 RTSP 输入端无关。抓包中的 4,344 个视频 RTP 包和 1,903 个音频 RTP 包均无时间戳回退；音频 SR 的 RTP/NTP 对应关系却反复跳动约 21 ms。原 600 秒 RTSP UDP 输入测试出现 67 次音频 PTS 回退，不能记为稳定通过。

根因是依赖库每包用实际发送时间重置 RTP/NTP 关联，再据此生成 SR；成批到达和调度抖动被当成媒体时钟变化。RTSP 还在 packet callback 内、当前包统计更新前生成报告。仅把报告移到 mux input 返回后，40 秒实验仍有 3 次回退，因此该单独改动被撤回，没有当作修复提交。

修复保留依赖库的 RTCP 调度、计数和 SDES，在封装完成后，将 SR 的发送 NTP 按会话共享的 PTS/NTP 基准转换到对应轨道的 RTP 时钟。一个可选基准及轨道 clock rate 用来表达真实的跨音视频时钟关联；不修改媒体 PTS/DTS，不钳制回退，不改变 RTP payload、队列、fanout、关键帧或第三方代码。这符合 [RFC 3550 §5.1、§6.4.1](https://www.rfc-editor.org/rfc/rfc3550.html#section-6.4.1) 对共享参考时钟与 RTP 时间戳关联的要求。

新增 `media_lifecycle` 定向回归通过公开 SETUP/PLAY 和 sink 投递路径发送刻意延迟的 G711 媒体，在三个真实 SR 周期验证对应关系。将修复前 `291de64` 的 RTSP 实现编译链接到该测试，断言稳定失败；候选修复通过。此音频-only 组件测试不代表公共发布接口新增 audio-only 产品能力。

| 验证 | 实际结果 |
| --- | --- |
| RTMP → RTSP，连续 H264+AAC 40 秒 | 1,200 解码帧，音视频 PTS 回退 0 |
| RTSP UDP → RTSP，原循环素材 60 秒 | 1,794 解码帧，音视频 PTS 回退 0 |
| RTSP UDP → RTSP，原循环素材 600 秒 | 17,945 解码帧；17,947 条视频、25,840 条音频完整时间戳记录，回退均 0 |
| 600 秒时间线末点 | 视频 600.028 s，音频 600.000023 s；差 27.977 ms |
| 600 秒服务资源 | RSS 21,892–23,352 KiB；四段 median 23,186 / 23,168 / 23,204 / 23,188 KiB；VmSize 198,472–201,032 KiB；FD 31–32；6 threads；UDP drops 0 |
| 普通 / UBSan / ASan CTest | 各 10/10 PASS，包含新增时钟回归 |
| 普通 / UBSan / ASan 真实多协议 smoke | 全部 PASS；每种构建覆盖 RTMP、RTSP、WHIP 输入及五类播放输出、GB UDP/TCP active/passive 中继 |
| 普通 / UBSan / ASan RTSP pull | connected、tracks ready、双轨 FFmpeg 解码推进；invalid URL 400、create 201、错误 identity delete 404、delete 204、重复 delete 404 |
| H265+AAC / H265+G711U → RTSP | 各 8 秒、160 解码帧，音视频 PTS 回退均 0 |
| 本修复后的换源回归 | RTMP / RTSP / WHIP / GB UDP 各 3 代，旧 viewer 结束、HLS generation 隔离及 ENDLIST/retention 断言通过 |
| Sanitizer 运行日志 | ASan 与 UBSan 各检查 66 份 smoke/pull 日志，报告均为 0 |

600 秒检查后半段与独立端口上的回归并行，服务自身平均 CPU 约 0.0046 cores；这是时间戳和稳定性观察，不是新性能基线。RelWithDebInfo 为 `-O2 -g -DNDEBUG -Werror`，二进制包含 debug info、未 stripped。ASan 使用既有 Boost 1.92 静态 ucontext/ASan 依赖，未绕过 guard。Go test/vet 通过（两个包无测试文件）。证据根目录仍为 `/tmp/media_server_product_current-2a70363-dD0La6`，对应 `rtcp-test-{red,green}.log`、`rtcp-clock-*`、`rtcp-*-ctest.log`、`rtcp-smoke-*`、`rtcp-pull-*`；保留全部先前失败记录。

### 浏览器验证日志的同类误判

本阶段完整产品/codec 矩阵、真实客户端结果与已知验证边界见 [产品能力与真实互操作验证](product_support_verification.md)。

继续检查发现 `browser_webrtc_verify.py` 的 WHIP 下游解码统计也保留了旧的宽松正则。执行脚本中的实际表达式解析先前保存的交错日志，会把音频行归给视频并误报 4 次 PTS 回退；仅接受紧随 filter 前缀的完整 `n / pts / pts_time` 记录后为 0。修复仅影响测试解析，不修改媒体实现，也不跳过真实回退。

普通构建与 CTest 10/10 通过。Chrome 153.0.8010.52 真实 WHIP H264/Opus 发布、下游 FFmpeg H264/AAC 解码 30 秒通过：600 视频帧，视频/音频 PTS 回退均 0，DELETE 204 后重复 DELETE 404。该浏览器 fixture 仍由发布端 pause/resume 产生一次自然关键帧，没有添加服务端关键帧请求。证据为上述目录的 `browser-parser-red.json`、`browser-parser-ctest.log`、`browser-whip-parser-green/result.json`。
