# 媒体端口 ownership 与隔离环境验证

## 基线与环境

BASE_HEAD：`ce402a570c5a98b560795d84561b3f1e6f8c54d7`。开始时 main 工作区干净，HEAD 等于 origin/main。

实现提交：`07cf46e622a4b41359fe9e1460947f429b63be60`（简化媒体端口分配与归还）。
FINAL_HEAD 为本报告所属提交，可用 `git log -1 --format=%H -- docs/media_port_pool_verification.md` 获取。报告提交不改变受测代码。

**构建机是 gyl@172.20.45.187，测试机是 root@172.20.63.32。** 通过前者 SSH 跳转到后者；本报告的隔离验证全部在 63.32 执行。曾在构建机运行的 loopback smoke 仅为附加回归，不代替 namespace 验收。

测试机证据目录：`/tmp/media-port-pool-07cf46e-A7VzoK`。二进制、客户端及 FFmpeg 生成的 H264/AAC fixture 从构建机复制，SHA256 逐项校验一致。必要动态库放在测试目录的 `lib/`，只通过进程级 LD_LIBRARY_PATH 使用，不覆盖系统库。Python/CMake 测试工具同样位于私有目录。

构建、系统调用跟踪及备份证据：`/tmp/media_port_pool-ce402a5-R4O23O`，其中 `test-host-63.32/` 保存测试机日志副本。机器可读结果及证据 SHA256 见 [media_port_pool.json](verification_results/media_port_pool.json)。

## 最终职责

```cpp
static void init(int start_port, int end_port);
static media_port_pool& instance();
std::optional<std::uint16_t> acquire();
void release(std::uint16_t port);
```

pool 只管理偶数 base port P 的 ownership：`available_ports_`、`used_ports_` 两个 vector，加互斥锁。只存 P，不保存 P+1。默认范围为 49152–65535，共 8192 个完整 allocation。

奇数起点向上调整到偶数；没有完整 P/P+1 或范围非法时初始化失败。acquire 从 available.back() 取出，放入 used；耗尽返回 nullopt。release 仅将确实在 used 中的 P 放回 available；重复、奇数或范围外 release 不做操作，不产生重复 entry。

删除 pair 类型、两套 reserve、bind 地址/socket/error_code、候选扫描、EADDRINUSE retry 和 fallback。没有兼容 API，也没有 reservation wrapper 或新增 session bool。

OS 负责让媒体范围避开自动 ephemeral 分配；pool 负责进程内 ownership；session 负责 bind 和失败清理。OS 预留不能阻止其他进程显式绑定，冲突时本 session 必须失败，不尝试另一个 allocation。

## 调用方

| 调用方 | 本地 allocation | socket 使用 | 失败与关闭 |
| --- | --- | --- | --- |
| WHEP、WHIP | 单一 local_port_ | bind P，P+1 不使用 | 先 UDP shutdown，再 release(P) |
| RTSP UDP publish | 每 track 一个 optional local_port | RTP=P，RTCP=P+1 | partial failure 关闭两个 transport 后归还；SETUP 失败 |
| GB UDP receiver | optional local_port_ | RTP=P，RTCP=P+1 | 关闭两个 transport 后归还；startup 返回失败 |
| GB UDP sender | optional local_port_ | RTP=P，RTCP=P+1 | 原 cleanup 先关闭 transport，再归还/reset |

RTSP SETUP 的 server_port 仍为 P-(P+1)。WebRTC 无第二份端口状态。没有修改 TCP、udp_transport 职责、media_stream fanout、队列容量、third 或 signaling。

## focused tests 与不重试证据

新增 4 个独立 CTest case，避免给 singleton 添加 reset API：

- 10000–10007 只分配 10000/10002/10004/10006；耗尽后 nullopt。
- 奇数起点、65535 上界、无完整 pair 和非法范围。
- release/reacquire、重复与非法 release、8 线程各 1000 次 acquire/release；无同时重复 ownership 或泄漏。
- 50000–50003 小池：五个调用方分别遇到 50000 显式占用后直接失败；RTSP 与 GB 收发额外覆盖 50000 成功、50001 失败。随后两次 acquire 得回 50000/50002，第三次耗尽，并实际重新 bind 验证关闭完成。

构建机 `bind-failure.strace` 记录 5 次 P 冲突、3 次 P+1 冲突；session 失败路径不尝试 50002，之后测试主动 bind 50002 成功。测试机 namespace 内，普通/ASan/UBSan 的四个 focused case 全部再次通过。没有 production debug hook。

## namespace 与 OS 预留

```text
172.20.63.32 host: ms-host 10.200.0.1/24
                     |
media-test namespace: ms-ns 10.200.0.2/24
```

创建前无同名 namespace/veth。未修改已有 media-test0（198.18.0.1/24、198.18.0.2/24）。双向 ping 各 3/3，丢包 0%。

namespace 内实际配置：

```text
net.ipv4.ip_local_reserved_ports = 49152-65535
net.ipv4.ip_local_port_range = 32768 60999
```

临时 probe 同时保持 10000 个 bind(10.200.0.2, 0) socket：10000 个唯一端口，reserved hits=0；随后显式 bind(10.200.0.2, 49152) 成功。仅测试进程提高 FD 上限，不改变系统级限制或 host 的 reserved sysctl。

namespace 保留供复现，测试后其中无存活进程。一次性 probe 位于证据目录，未加入生产仓库。

## 构建与普通回归

RelWithDebInfo 实际包含 `-O2 -g -DNDEBUG -Werror`，主二进制带 debug_info、未 stripped。通过 CMake 选项启用 Boost 1.92 静态优先、OpenSSL 静态优先和 pkg-config --static；没有为依赖修改 CMake 源策略。不宣称整个 ELF 完全静态，实际动态依赖见 normal-ldd.log。

构建机普通 Debug、RelWithDebInfo、ASan、UBSan 全量 CTest 均 14/14。测试机复制原测试二进制、脚本及 CTest manifest，仅调整临时 manifest 的文件路径和 Python 可执行位置，namespace 内全量 CTest **14/14 PASS（23.52 秒）**，包括 UDP transport、media lifecycle、H264/H265 RTMP 解码及 CLI 错误配置。

namespace 普通 smoke 使用实际 CLI：

```text
--bind-address 10.200.0.2 --webrtc-address 10.200.0.2
--threads 6 --rtmp-port 45330 --rtsp-port 45331 --http-port 45332
```

基于现有 lifecycle_verify.py，临时 wrapper 只替换测试 host 常量；没有改变生产路由或协议。服务与客户端均在 namespace 内，使用 10.200.0.2 而非 loopback；跨 veth 连通性另由双向 ping 验证。

RTMP、RTSP UDP、WHIP 三类输入均验证 1/4 个 RTMP、RTSP、WHEP、HTTP-FLV、HLS viewers，以及 GB UDP/TCP active/passive 转发；RTMP/RTSP 输入另做 FFmpeg 双轨解码。所有用例持续推进，runtime failures=0，异常 queue-full=0。不进行 capacity 测量。

## 端口释放与重用

临时 port_lifecycle.py 通过服务进程 FD 对应的 socket inode 过滤 /proc/net/udp，核对实际绑定地址、端口；不是仅看 registry 或日志。每条路径连续两轮：运行实际媒体，关闭 session，等待服务 socket 消失，显式重新 bind 原端口，关闭 probe socket，再创建 session。

| 路径 | 实际端口 | 验证 |
| --- | --- | --- |
| WHEP | 每轮 49152 | ICE/DTLS/SRTP、视频音频推进，关闭后 P/P+1 可绑定，下一轮重用 P |
| WHIP | 每轮 49152 | 真实 RTP 发布、viewer 推进、DELETE，关闭后 P/P+1 可绑定并重用 |
| GB UDP receiver + sender | 49152/49153、49154/49155 | 收发转发与 viewer 推进，关闭后四个端口可绑定并重用 |
| RTSP UDP H264/AAC 双轨 | 49152/49153、49154/49155 | SETUP server_port 与实际 socket 一致，关闭后可绑定并重用；第二轮 track 分配顺序可互换 |

普通、ASan、UBSan 各 **8/8 PASS**；按集合验证 allocation 重用，不要求不同 track 的分配顺序固定。

## Sanitizer

ASan 使用已验证的静态 Boost 1.92 ucontext 构建，现有 guard 正常通过；没有禁用 guard、修改 Boost/third 或添加 workaround。运行 `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`、`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`。

测试机上两种 sanitizer 均覆盖上述五条 UDP 路径的真实媒体、关闭、端口重用与 bind partial failure；ASan 还单独通过 UDP transport 生命周期/overflow/socket-error 测试。ASan/LSan 报告 0，UBSAN_REPORTS=0。保留 Boost makecontext/swapcontext 的已知提示；普通 clients 与部分预编译外部依赖没有插桩，不声称全依赖覆盖。不新增 TSan 工作。

## 最终范围与搜索

生产 media/** 增加 143 行、删除 241 行，净减少 98 行。测试/CMake test wiring 单独计入，不以行数代替 correctness。

旧 acquire_and_bind、acquire_pair_and_bind、port_pair、reserve_pair、next_pair_port 在生产代码中为 0；pool 内 address_in_use 为 0。五处 acquire 及全部 release 已核对：没有重新 acquire 的 bind retry，成功、partial failure、正常 shutdown 均先关闭 socket 再 release。

实现阶段已提交并推送；本报告与 JSON 作为独立文档阶段提交。最终 Git 核验要求 clean、HEAD==origin/main，third/signaling/fanout 无改动。
