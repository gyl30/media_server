# 大华广播入口与 PCMA 静音网络取证

## 1. 范围与结论

2026-10-16，代码基线 `0a6e2a553cbb4c6af9a23413487cc55206374911`。本轮为广播/对讲实施前的真实设备取证，不是 Qt 广播功能交付。生产代码、既有视频 PS sender、桌面 SDK 和相机配置均未修改。

实测确认：摄像机接受主设备目标的 Broadcast 通知，返回独立 XML `Result=OK`，反向发起音频 INVITE；修正私有探针后完成真实 200/ACK、10 秒 PCMA 近零静音 UDP 写出和正常 SIP BYE。没有收到相机 RTCP，不能据此证明设备收包、解码或扬声器播放。Qt 麦克风、跨主机媒体来源、实际听音与完整双向对讲仍未验证，整个阶段六及平台仍未 GO。

## 2. 设备、标准与身份

设备是 `172.20.44.240` 的 Dahua `DH-SD-P3A1205-P`。SIP 仍指向专用测试机 `172.20.63.32:15060`，没有恢复旧平台。

| 身份 | 本轮值及用途 |
| --- | --- |
| 主设备 / Broadcast TargetID | `34020000001320000089` |
| 真实 Catalog 中的视频通道 | `34020000001310000089` |
| 平台 SIP ID | `34020000002000001270` |
| 本轮平台自有临时语音输入 SourceID | `34020000001360000001`，type 136；不是伪造摄像机通道 |

已保留的 Catalog API 证据只有一个 type 131 通道，不能由此判定设备无广播能力。本轮未伪造 type 137。

核对的是 GB/T 28181—2016，不声称已验证 2022 全部差异：

- §9.12.1.1，印刷页 45：允许用前端主设备作为目标，语义覆盖该设备的语音输出。
- §9.12.1.2–1.3，页 47–49：音频接收设备反向 INVITE；MESSAGE 事务成功与独立广播业务响应是不同事实。
- A.2.5 d / A.2.6 l，页 64 / 74：Notify 使用 SN、SourceID、TargetID；独立 Response 使用同 SN、DeviceID、Result。不能因此移除现有所有 MESSAGE 的 DeviceID 校验。
- C.2.4、D.1、附录 K，页 81 / 84 / 195：PCMA 音频、136/137 编码和 Subject 双方身份。Subject 中的媒体序列字符串不能机械当成十进制 SSRC。

本地标准 PDF：`/var/tmp/qt-codex-resume-tRclw0/GBT28181-2016.pdf`，SHA256 `6b750d21c2382e129379ebc64147bb37e3a6d47acf4e36c51df8ea580bf6bd17`。这里只引用章节与转述，不复制整段标准。

## 3. 可复核证据与安全边界

DEV：`/var/tmp/qt-dahua-broadcast-0a6e2a5-dH6hx0Gk/`。

TEST：`/var/tmp/qt-dahua-broadcast-0a6e2a5-V8rCVas2/`。

独立 Go overlay 测试复用生产 REGISTER/Digest/Catalog 和 MANSCDP 发送；新增的广播应答、UAS 和 PCMA/RTCP 构造均是私有探针，不冒充生产广播实现。执行机为 TEST，未启动生产媒体服务器或 Qt。SIP 凭据来自此前确认的仓库外 0600 文件，未输出密码或认证头。

每轮保留 `manifest.json`、精确 ELF SHA、PID/starttime、命令、退出码、原始日志和事件；失败轮次不覆盖。DEV 另存源码、overlay、构建日志（成功编译的空输出）、Claude 审查、`analysis.json` 和收口生成的 SHA 清单。私有目录不提交 Git。

摄像机 Web 只执行 GET：读取型号、AudioOutput、AudioOutputVolume 及相关能力端点。成功返回 `Speaker`、音量 `97`；两个未知配置名称返回 HTTP 400，原失败保留，不能当作音频编码不存在。前后已查询的响应内容相同；未写配置，因此没有需要恢复的相机改动，也不据局部 GET 声称核对过所有配置。平台不发送可听测试内容，不能保证设备自身没有提示音。

## 4. run-01：通知和反向呼叫拒绝

私有测试 `TestPrivateDahuaBroadcastEvidence`：6.969 秒，退出 0。这个退出码仅代表取证流程/清理断言，不代表广播媒体成功。

- 真实注册和 Catalog 后，发出一次应用层 Broadcast Notify：主设备 TargetID、本轮 SourceID、SN=2。
- `sendMANSCDP()` 成功只记录了对方最终 **2xx**，探针没有保存该事务的具体响应码，不能把它补写成已证实的 200。
- 独立 Response：同 SN、DeviceID=主设备、`Result=OK`，注册来源核对通过后回 200。
- 收到反向 INVITE：Request-URI/To 指向本轮 SourceID，From 为真实主设备；Subject 双方身份吻合，其序列是非十进制字符串。
- SDP：`s=Play`、`c=172.20.44.240`、`m=audio 9764 RTP/AVP 8 96`、`a=recvonly`、PT8 `PCMA/8000/1`，同时提供 PT96 `PS/90000`；`y=0259058970`。
- 平台仅发送 488 拒绝，不分配媒体资源，不发 RTP。日志有一次 `ACK missed`，表明匹配事务的 ACK 未被消费；没有保存该 ACK 头部，不能称探针已完成独立 ACK 校验。

## 5. run-02 → run-03：私有探针 RED/GREEN

run-02 用 sipgo `DialogUA.ReadInvite` / `RespondSDP` 创建音频 UAS，50.444 秒退出 1，原始失败为 `real ACK timeout`。`answer_join` 返回 `transaction terminated`，无法确认 200 是否真正发到网络。没有发送媒体；RTP/RTCP 端口关闭后均能重绑定。

代码审查发现两个探针问题，均未修改生产或依赖：

1. INVITE handler 只把 dialog 投给主流程便返回，稍后才异步发送 200。sipgo 在 handler 返回后执行 `TerminateGracefully()`；尚未发 final 的 UDP transaction 会被终止，存在实际启动竞态。
2. 只监听顶层 `OnAck` 没覆盖库的两种真实 ACK 路由。匹配旧 INVITE transaction key 的 ACK 进入原 `tx.Acks()`；新 key 才进入 `OnAck`。不能靠伪造 `ReadAck` 解锁。

run-02 的日志只有 `ACK missed`，没有该 ACK 的完整取证，**不能认定它是 200 的 ACK，更不能认定大华复用了 2xx ACK branch**；它也可能来自重传 INVITE 遇到探针已有 dialog 后的非 2xx 拒绝。

修正私有探针：在 INVITE handler 内拥有响应直到完成；同时消费原事务真实 ACK 和顶层 ACK，统一检查完整 dialog ID、来源、CSeq、终态后调用 `ReadAck`；有界 join、取消、错误与证据文件关闭均保留。

依赖调用链证据（固定 sipgo v1.6.0，根路径 `/home/gyl/go/pkg/mod/github.com/emiago/sipgo@v1.6.0/`）：

- `server.go:245–255` → `sip/transaction_server_tx.go:168–183`：handler 返回和提前终止。
- `sip/transaction.go:306–356` → `sip/transaction_layer.go:228–251` → `sip/transaction_server_tx_fsm.go:69–74,227–229` → `sip/transaction_server_tx.go:130–155`：两条 ACK 路由，`ACK missed` 是交付未消费，不等于网络没有收到 ACK。
- `dialog_server.go:24–30`：`ReadAck` 自身只校验 CSeq，调用方仍须校验 dialog identity/终态。
- `dialog_server.go:258–351`、`dialog_ua.go:61–92`：200 等待没有业务 ctx；`Close()` 不是取消/终止事务。循环内 ACK 超时计时器有局限，但真实 transaction 另有 Timer L，不能笼统称无限等待。取消时必须保留原 transaction 与最终响应工作归属，不能遗弃 goroutine。

## 6. run-03：真实 UAS、静音发送与清理

`TestPrivateDahuaBroadcastMediaEvidence` 修正后 51.468 秒退出 0，其中媒体墙钟 10.001141338 秒。本轮 SDP 的远端 RTP 为摄像机 `9772`，answer 为 TEST `38442`、显式 RTCP `38443`、`sendonly`、PT8 PCMA；私有探针沿用此次 offer 的 `y=0259728250`，按源码构造 RTP/RTCP 的 SSRC。`media_start` 记录 259728250，事件未逐包记录 SSRC。这只是已测 profile，不要求所有合法初始 offer 都必须带 y。

| 结果 | 实际证据与边界 |
| --- | --- |
| 独立 Broadcast Response | 同 SN，`Result=OK` |
| 200/ACK | `RespondSDP` 返回成功；2 条真实 ACK 均进入顶层 `OnAck`，CSeq=`1 ACK`，与 INVITE 的 Via branch 不同 |
| PCMA 数据 | 500 包，每包 160 字节 `0xd5` 近零静音，共 80,000 样本，8kHz，10 秒媒体 |
| RTP 时基 | seq 10000–10499，timestamp 270544960–270624800，逐包增量 160；本地记录序列/时戳步进错误均 0 |
| 定时发送 | 相对计划发包时刻的本地 lateness 中位数 0.634ms、最大 1.168ms；不是网络 jitter |
| RTCP 发送 | SR/SDES，结束时 compound SR/SDES/BYE，本地 UDP 写成功 |
| 相机 RTCP | 0 个 datagram；空原始文件仍保留，不据此宣称相机丢包为 0 |
| SIP 结束 | `DialogServerSession.Bye` 成功；该库路径要求最终 200 |
| 清理 | 自有响应/接收任务有界 join，38442/38443 可重绑定；进程已退出，15060 可重绑定，无遗留本轮监听 |

远端 SDP 没有显式 RTCP 属性，探针按相邻端口 `9773` 发送；这是缺省端口规则下选择的发送目的，不是收到相机 RTCP 后确认的实际端点。UDP Write 成功只证明本机内核接受，不能替代设备收包或解码。正常轮仍有未匹配 SIP 200 的库诊断，原日志保留，未据此推断具体重传次数。没有抓取或提交认证原始报文。

## 7. 后续候选方案与验收门槛

首发 Linux 直连 LAN 的候选方案 A：**Qt 充当真实语音输入、直接向相机发送 PCMA RTP/RTCP，Go 只处理 SIP**。标准角色不强制媒体绕行 C++ 进程，不先把视频 PS sender 泛化成纯音频或增加无依据的中继。该方案尚未实现，以 Qt 与 Go 分处不同主机时设备实际收音为采纳门槛；门槛未过前不排除服务端中继方案。此前接手审查中复用服务端 G.711 RTP/UDP 的建议不是已实施的约束。

必须完成的条件：

- Qt 实际绑定/公告自己的媒体地址，Go 和 Qt 在不同主机，确认相机接受非 SIP 平台 IP 的媒体来源；本轮 RTP 和 SIP 都来自 TEST，**尚未验证此条件**。
- Qt 单一音源 owner 持有采集、重采样、PCMA 编码、RTP/RTCP；现 SDK 无 PCMA encoder/RTP mux，必须使用新独立前缀追加，不能覆盖旧 SDK。复用 FFmpeg，不自写正式 G.711/RTP 编码器。
- Go 一个广播 owner 关联 SN、临时输入身份、dialog 与客户端存活租约；一个目标同刻只认领一个广播。Go 租约到期 BYE、Qt 与 Go 控制连接失联时 Qt 自停，正常 DELETE/设备 BYE/停机按对象 identity 清理，不复制一份媒体源状态。多个客户端不能共用静态临时输入身份。
- 允许业务 Response 与反向 INVITE 的异步到达；真实 ACK 前不能发媒体；取消/迟到 INVITE/201 写失败必须收口，不能依靠 `Close()` 或 HTTP ctx 自动结束 SIP。
- FFmpeg RTP mux 的 SSRC/PT/packet boundary、RTCP输出和接收处理须分别测试；没有 RR 不能说完整验证 RTCP。旧播放/录制/下载须对新 SDK 重新回归。
- 实际麦克风、设备声音、上下行同时运行、回声风险及退出/失联均需要真实验证。没有 NAT/TURN、TCP 音频媒体、多目标同时广播等验收，不把这些边界写成通用 GB28181 能力。

Claude Code 只读设计复审未发现本次取证阻断；后续正式设计中的具体 API、租约和取消时序仍须按实际实现复审。审查中关于 ACK 等待、Record-Route、初始 y 和 FFmpeg SSRC 类型的简化说法不当作源码事实：以锁定库实现及上面的实测为准。

## 8. 校验与未执行项

| 文件 | SHA256 |
| --- | --- |
| run-01 ELF | `604e443235f6ceaa725a513a492b067a86793e1756f8f5ecafba41236f771153` |
| run-02 ELF | `2cf7e0708be9a2fb46116e3b83d87fce38d1ee7cb7421a5550a715afe6d8785f` |
| run-03 ELF | `1464966f4b487f3d2a8d4493bf53c7083aac07c802d203f39cc110c2a370e2f8` |
| run-03 events | `797d47d9dd7ec33241ede0c3a9041b935971d263bbc947fd63ac53daef646dc2` |
| analysis.json | `1a65d22a478e2a7e72ce8afaf012c3cee7894c64b0900113913ef773e8cae534` |

三份 overlay 编译通过；运行结果依次 0 / 1 / 0。只编译/执行指定私有测试，未执行当前 HEAD 全量 Go test、vet、race、C++/桌面 CTest、ASan/UBSan，不继承此前 PASS。Qt 广播、麦克风输入、相机解码/听音、跨主机直送、完整对讲均为 NOT_IMPLEMENTED 或 NOT_RUN。清理仅限本轮可再生草稿；原始失败、源码、ELF、日志、配置快照与审查材料保留。
