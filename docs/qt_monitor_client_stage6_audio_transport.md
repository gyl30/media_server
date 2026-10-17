# 阶段六增量：媒体侧 PCMA 音频发送后端

## 范围与基线

2026-10-17 从 `21949b9ac1ca2495b17d10da6aa1f6b596a79905` 接手 `feature/qt-monitor-client`。原远端项目 Codex 已于当天 12:39 结束，无需终止其他 Codex/Agent 进程。仅修改 `/home/gyl/example/media_server_qt`，不改 main、第三方或正常 Boost 安装。

用户最新范围是 Qt/Web 麦克风直达媒体服务器、媒体发送设备侧 RTP、Go 仅控制。此增量实现可被后续上传会话持有的 `gb28181_audio_sender`，不是完整对讲功能，也没有新增独立广播 UI。

## 实现契约

- 全部资源操作在 owner worker。`startup(bind_address)` 只绑定端口池给出的 RTP/RTCP 端口对；调用者以真实端口执行既有 SIP 协商，成功后调用一次 `set_peer(rtp, rtcp, ssrc)`。
- 两个阶段有真实协议依赖：反向 INVITE 成功之前不知道设备端点和 SSRC，但 SIP Answer 已经需要本地绑定端口。不是 prepare/activate 防重状态机。
- `input_audio` 接收每包 160 字节 PCMA/8000/mono（20ms）；上层负责实时采集节奏。PT=8、每包时间戳增加 160；UDP 入队溢出丢新包，不回退时间戳/sequence，不冒充 socket 错误。
- 沿用现有 ireader common RTP encoder、UDP 有界队列及其异步 buffer ownership；不经 MPEG-PS，不改 WHIP 或通用媒体核心。编码回调同步读取原始 packet，transport 复制并持有实际在途数据。
- RTCP sender 使用 8000Hz，周期 5 秒，包含 SR 与非空 CNAME 的 SDES；计数只包含已入队 RTP。接收端仅检查来自协商端点的 compound RTCP 边界并处理 BYE，不引入未使用的 XR/反馈解析。
- 同步失败直接返回，由父对象请求 `shutdown()`。实际异步网络错误与 peer BYE 先移出错误回调、投递自身关闭，再通知父对象。正常主动关闭/worker 取消不伪造错误。
- `shutdown()` 始终 post 本对象 `safe_shutdown()`。无条件清父回调和订阅，取消 timer/读、关闭两个 socket、销毁 packetizer/RTCP、只归还自己持有的端口。没有新增 started/closed/closing/原子状态。

## 已确认问题与 RED/GREEN

本次新增通道最初直接调用第三方 `rtp_onreceived_rtcp()`。只读复审发现当前锁定依赖 `third/ireader/librtp/source/rtcp-xr.c` 的 XR block 循环在零长度未知 block 上不前进。通过已配置的真实 loopback RTCP peer 输入该报文，测试在明确的“checking owner progress”标记后挂住，由外层 16 秒 timeout 终止，退出 124；此前媒体及关闭断言通过。

最小修复只发生在新增通道：不调用不需要的通用 RTCP parser，只做有界 compound header/BYE 检查，完整报文有效后才处理 BYE。补测未知 XR、SSRC 数量不足、截断 compound 尾部、非法 padding、越界 BYE reason；无效输入忽略，随后有效 peer compound BYE 仍能关闭。没有修改第三方或扩大审查为其他模块修复。

关键证据位于 DEV `/var/tmp/qt-audio-sender-21949b9-xGQL3x5w/`：

- `rtcp-red2.exit`、`rtcp-red3.exit` 为 124；`rtcp-red3.log` 包含到达目标输入的标记。
- `rtcp-green.exit` 为 0；`rtcp-green.log` 为修复后七组真实网络/生命周期断言。
- `rtcp-red.exit` 的 1 仅是测试补丁尚未落盘时的前置搜索失败，不算 RED。
- `ctest-full.log` 保留首轮并行/自动工具路径下 65/70：系统 FFmpeg 不支持既有 HEVC/FLV fixture，另四项 WHEP 测试出现默认端口占用。没有改动测试断言；后续使用项目 CLI `/home/gyl/bin/ffmpeg` 和串行 CTest。

## 回归结果

| 验证 | 实际结果与证据 |
| --- | --- |
| RelWithDebInfo 完整构建 | PASS，`build-final.log`，GCC 16.0.1 experimental、Boost 1.92 静态、`/home/gyl/ffmpeg901` SDK；保持 `-O2 -g -DNDEBUG -Werror` |
| 完整串行 CTest | PASS，70/70，471.21 秒，`ctest-serial.log`；使用 `/home/gyl/bin/ffmpeg`，既有 TCP/UDP 发布和 RTMP/RTSP/WHEP 实际媒体测试包含在内 |
| ASan 兼容守卫 | PASS，`asan-configure.log`，独立目录真实探测通过；复用既有独立 Boost 1.92 ASan 前缀，不改安装 |
| ASan+UBSan 定向生命周期 | PASS，3/3，16.24 秒，`asan-directed-final.log` 和 `asan-directed-detail.log`；覆盖新音频 sender、既有 GB sender、UDP transport |
| LeakSanitizer | 同一次定向运行启用 `detect_leaks=1`，退出 0，无泄漏报告；保留 Boost ucontext 的 ASan 已知提示，不隐藏诊断 |
| Claude Code 只读复审 | 最终 `claude-final-review.json` 未发现 P0/P1；建议的合法 BYE reason/padding 和零来源 BYE 用例已补齐并在定向 sanitizer 重跑 |

新 `gb_audio_sender` 测试最终包含八组真实环回/生命周期场景。完整套件后，追加的合法 BYE 正向断言单独重跑；这不是新增一轮容量或全套 ASan。Go、Qt/Web 源码没有修改，本阶段不把旧 Go/桌面通过结果继承为新端到端对讲 PASS。

ASan 前缀为 `/var/tmp/media_server_phase7_asan_23e77ca_zFCjAKc8/boost-1.92-asan/`，实际项目编译宏为 `BOOST_USE_ASAN`、`BOOST_USE_UCONTEXT`，插桩为 `-fsanitize=address,undefined`。新第一方代码和 ireader 参与插桩；普通 FFmpeg/OpenSSL 等外部库未因此变成完整插桩版本。没有运行完整 ASan CTest、Qt/Web 音频或实机出声验收。

## 仍未交付的部分

- 上传 HTTP/WebSocket 路由及一次性接入认领；此组件没有公开网络授权接口，不能被当成已完成的产品入口。
- Go SIP owner 与媒体 owner 的结束闭环、上传取消/断开及设备下线清理。
- Qt/Web 麦克风采集、重采样/A-law、实时拥塞处理，以及 Web 安全上下文/WSS 部署。
- 设备扬声器真实出声、设备麦克风返回与双向通话体验。当前环回测试不替代物理/真实设备音频验收。
- 设备录像 Scale、同会话 seek、解码和 PCM 队列清理及进度展示。

现有 Qt 观看、实时本地录制和历史实机失败记录均保留。本阶段不是完整平台 GO，也不是公开发布许可验收。
