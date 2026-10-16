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

## 9. 独立音频 SDK 前置验证

后续在 `755cf52` 源码上准备独立 FFmpeg **9.0.1** shared SDK：`/var/tmp/qt-broadcast-sdk-755cf52-z2y4wEha/install`。使用现有 stage1 源码、GCC **16.0.1 experimental**，沿用原配置，仅追加 `--enable-encoder=pcm_alaw`、将 muxer 改为 `matroska,rtp`；未覆盖原 `/var/tmp/qt-claude-owner-work/ffmpeg-mkv/install`，其旧 SHA 清单全部复核一致。未修改项目 CMake、业务代码或默认 SDK 选择。

新旧 `config_components.h` 只差 PCM_ALAW_ENCODER 与 RTP_MUXER；启用首个 encoder 自动打开的 CONFIG_ENCODERS/FRAME_THREAD_ENCODER 不代表增加外部编码依赖。保留 `--disable-gpl --disable-version3 --disable-nonfree`；LGPL 2.1+ 许可与 LICENSE 文本放入新 SDK，不据此声称整个 Qt 发布包的许可已经闭合。测试素材生成 CLI 仍是独立的 `/home/gyl/bin/ffmpeg`（n7.1.1-57-g1b48158a23-20250824），不链接或捆绑到 SDK，其版本、配置和 SHA 另存 `analysis.json`。

私有探针 `pcma_rtp_sdk_probe_v2.cpp` 使用真实 FFmpeg PCMA encoder、RTP mux 和 PCMA decoder：custom AVIO buffer=4096、max packet=1200、`direct=0`，每次 callback 同步复制完整 datagram。没有自写正式编码器/packetizer，也没有新增 Qt 生产类或后台任务。

| 验证 | Normal | ASan+UBSan | 实际边界 |
| --- | --- | --- | --- |
| 50×160 样本编码、RTP、回解码 | PASS | PASS | 50 包、8000 样本；1 秒媒体数据，非实时 1 秒发包 |
| 单帧 2560 样本分片 | PASS | PASS | 3 包，负载按 mux 分片为 1188/1188/184 字节；PT8、SSRC、seq、8kHz 时戳步进检查 |
| SR/SDES/BYE | PASS | PASS | 两份 compound RTCP，最终计数正确；短时内存输出，不验证长期周期、NTP 映射或 RR |
| 第 5 个 callback 注入 EIO | PASS | PASS | 明确检测 `pb->error=-5`，收尾返回 -5；不把模拟 I/O 失败当作成功 |
| PCM 量化误差 | 64 | 64 | 实测最大绝对误差，断言阈值 256，不是无损音频 |
| 当前桌面完整 CTest | 8/8 | 8/8 | 170.73 / 173.09 秒；各 422 项 QTest，0 failed、0 skipped |

第一次桌面执行遗漏 `FINITE_PS_TEST_FIXTURE_DIR`，虽然 CTest 返回 0，但每模式只有 373 项通过、49 项跳过，明确记为 **INCOMPLETE**，原日志保留；指定既有 `fixtures-v2` 并记录素材 SHA 后重跑才得到表中完整结果。回归包括实际 fixture 解码、有限 PS 呈现、暂停、录制/下载、错误与退出，但不是再次运行物理摄像机。链接检查确认桌面和探针实际加载新前缀的库。

ASan/UBSan 插桩的是探针和桌面代码，`detect_leaks=1:halt_on_error=1` 保留，无诊断；本次 FFmpeg SDK 本身未插桩，不能扩大为其内部全量内存安全验收。同步内存 datagram 收集不验证 UDP 队列、慢网络、采集设备、实际 pacing 或相机声音。本增量没有运行全量服务端 CTest、Go test/vet/race、Qt 内部包或跨机广播。

证据根目录 `/var/tmp/qt-broadcast-sdk-755cf52-z2y4wEha/` 保留构建/运行脚本、两版探针、精确 ELF、首轮跳过日志、最终完整 QTest 日志、配置及依赖校验。Claude Code 与独立 Codex 只读复审未发现本增量 P0/P1。仅清理该目录自己生成的 FFmpeg build 中间对象：目录从 63,290,401 降至 1,018,833 字节，安装产物重新校验全部一致；正常/ASan 桌面构建留作后续局部增量复用，不删除原始证据。

| 证据 | SHA256 |
| --- | --- |
| `analysis.json` | `be23a0a5e60023082028b7726ced8fd0125e6698ae0bdc6c7699b750295208f9` |
| `install-with-licenses.sha256` | `7782a739277a60268d5d43590f9122e1bf9e7014009e3be2aa4cc2d638faa9d3` |
| `pcma_rtp_sdk_probe_v2.cpp` | `b278153f061acd7f8438feb9bb7e4b05c18da1f96638450117d9985bf9e32b6f` |

结论仅为音频 SDK 的前置能力与旧桌面路径回归通过。Qt 音源 owner、Go 广播 owner/API、控制失联清理、跨主机设备音频路径及真实听音仍须继续完成，不改变前述候选方案和平台未 GO 的结论。

## 10. 跨 IP 广播与相机回传音轨取证

在 `5efffbf` 上执行一次私有实验，run_id=`qt-crossip-5efffbf-01`。SIP 留在 TEST `172.20.63.32:15060`，PCMA/RTP 真实发送进程位于 DEV `172.20.45.187:38442/38443`，反向 INVITE 的 answer 公告 DEV 媒体地址，Contact 仍为 TEST。实时视频使用生产 `live.startLive()` 的 UDP 点播，TEST `38440/38441` 私有 collector 保存 PT96 PS，不伪造历史会话以绕过 TCP 校验，也不把 collector 当生产 C++ receiver 验收。

证据目录：DEV `/var/tmp/qt-dahua-crossip-5efffbf-tVvuJa0W/`，TEST `/var/tmp/qt-dahua-crossip-5efffbf-HwhjcfYU/`。运行前的 `predeclared-method.md` 固定低电平五段非谐波音序列、6秒前后采集、分析阈值及停止条件；没有因观察结果更改素材、电平或判据。CLI、原包、PS、PCM、PTS、两机单调 trace、原始失败与 SHA 清单保留，未经脱敏不公开原始运行资料。

| 项目 | 实际结果与边界 |
| --- | --- |
| SIP 广播 | Notify 最终2xx、独立 Result=OK、反向 INVITE/200/真实 ACK、正常 BYE；ACK 为顶层路由 |
| 跨IP本地发送 | 400 RTP ×160 PCMA样本，共64,000样本；墙钟8.000209秒；实际SSRC 262506130，seq与timestamp步进错误均0 |
| 定时 | 本地计划 lateness 中位0.164ms、最大0.386ms；不是网络jitter或相机播放延迟 |
| RTCP | FFmpeg生成3份compound RTCP并本地写出；DEV RTCP端口收到0包，不等于设备丢包为0 |
| 实时回传 | 10,261 RTP、11,921,331 PS字节；实际SSRC 200000001，保存序列1–10261连续，无观测序号缺口 |
| 视频解码 | H.264 1920×1080；FFmpeg退出0，577帧，视频stderr空 |
| 音频解码 | PCMA/8000/mono；384帧、184,320样本、23.04秒；FFmpeg退出0，但有两次重复DTS警告，不能称干净无警告 |
| 源回解码 | 64,000样本，最大绝对PCM量化误差11；原PCM峰512，不是无损 |
| 清理 | probe 27.71秒通过、外层28.912秒；live/SSRC/collector登记均0，DEV和TEST自有端口可重绑定，两个PID已消失 |
| Sanitizer | DEV真实发送进程启用ASan+UBSan、LSan，stderr空；SDK本体及外部解码CLI未插桩，Go探针不是race运行 |
| 相机配置 | 前后查询记录一致，Speaker及音量未改；其他未知配置名返回400不推断设备不支持 |

**音频路径验收结果为 INCONCLUSIVE。** 原音频每帧480样本应占60ms，但PS音频PTS差值主要为40/80ms及其10ms偏移，另有2次重复PTS，最大相对样本时长误差70ms。帧样本总数与解码PCM一致，不能把PTS不均匀直接写成丢音。预先规定的3ms连续性前提不满足，原分析退出2并保留，未改阈值宣称通过。

另外保存的 `sample-domain-diagnostic.json` 明确为非验收诊断：忽略PTS前提后，全部五段统一偏移的包络相关仅0.637880（要求0.80），部分对应频带增强，但697/1229Hz包络及941Hz多项失败。此探索结果同样 INCONCLUSIVE，不能覆盖原结果，不能由部分频带增强宣称完整跨IP音频路径或物理扬声器验收通过。

另记录两次私有工具准备失败：首次未生成overlay文件，Go编译入口报缺文件（原输出仅在本轮执行记录，无独立原始日志）；一次非交互SSH的PATH找不到Claude，stderr文件保留，改用已存在的绝对路径后完成只读审查。这些不是媒体运行失败。Claude Code及独立Codex运行前复审无P0/P1；修正本轮supervisor异常清理、避免覆盖已发布的本地发送结果、授权剩余预算检查后执行，没有修改生产状态或相机设置。

| 文件 | SHA256 |
| --- | --- |
| Go probe ELF（DEV/TEST一致） | `3d19afe0d9bca8e881fada5537a754bc101dcc50cedb6e0a540a71b7a6c6f463` |
| ASan sender ELF | `373f607a3092ab01f7d2d7dd971d3f8b38c7930640a957102efb4f3939147cb8` |
| `analysis.json` | `d0e4dde708bc3267dda0598c2fcbb80aaff0b33d72e4c3ede08c0c10bc885fee` |
| 原判据 `audio-association.json` | `0ef99b148fc0ee36c0a866ad340a7e2d7c9d752ba8cde506674758dc9c5c850b` |
| 非验收 `sample-domain-diagnostic.json` | `61131916e14815552d4fc2fcefadf22a3ded594f9a91c206545afd2a35ed4c13` |

本轮没有实施 Qt 麦克风音源、Go 正式广播 owner/API，也没有人工听音或完整对讲；未运行全量服务端/桌面 CTest、Go test/vet/race。第9节的桌面回归不替代本次设备路径验收。候选A仍未达到既定采纳门槛，不新增中继或改写既有协议来追求全PASS。

## 11. 临时音量对照与失败恢复

用户授权按验证需要调整相机配置后，在 `ad3bc07` 上前瞻固定一次 A97→B0→A97 对照，沿用第10节精确 ELF、五频源、峰512和所有原分析门槛。只改 `AudioOutputVolume[0]`，不改平台地址、编码、录像或网络。每次 SET 均 GET 核实；没有提高信号电平。DEV `/var/tmp/qt-dahua-volume-ad3bc07-oNLeI97Y/`、TEST `/var/tmp/qt-dahua-volume-ad3bc07-yY56kBxj/` 保留方法、配置读写日志、原包、精确二进制、比较脚本及所有失败。

| 轮次 | 实际执行结果 |
| --- | --- |
| A-before，音量97 | Go 30.58秒通过、DEV外层31.851秒；400 RTP /64,000样本，真实ACK/BYE。采集10,551 RTP、12,228,485 PS字节，原包与trace及PS一致；H.264解码599帧、PCMA解码191,520样本/23.94秒，退出均0。视频stderr空、音频仍有重复DTS警告，原严格分析INCONCLUSIVE；非验收联合相关0.582122，不是全部五频通过 |
| B-muted，音量0 | SET返回OK并GET确认0；私有Go在120.00秒报 `registration/catalog deadline`，外层121.622秒失败。没有广播授权、没有发送RTP，实时PS与RTP记录均0；不能把这轮当成有效静音负对照 |
| A-after，音量97 | 按预定停止条件未执行，NOT_RUN，不补造恢复后的媒体结果 |
| 配置恢复 | 先确认本轮sender退出和端口释放，再SET97；连续GET均确认97，`experiment-status.json` 的 `restored=true`。这是配置恢复，不是A-after媒体复验 |
| 清理 | A/B两轮live、SSRC、collector登记均0；DEV/TEST自有端口均可重绑定，两个sender与两个Go探针PID已消失 |

整个实验为 **INCOMPLETE**，离线对照为 **INCONCLUSIVE**，比较器退出2并准确列出B和第三轮缺少的输入。B只证明120秒内没有同时满足“在线注册＋ON通道目录”条件；该探针未保存REGISTER/Catalog细分事件，不能断言完全未收到REGISTER，更不能据此判定摄像机广播或音频失败。逐轮销毁并重新启动空注册状态的SIP平台不适合直接充当连续音量实验；后续应使用稳定的单一控制会话，不改生产鉴权、不伪造在线设备，也不靠不断加长媒体期限或删除失败来获得结果。

私有恢复脚本修正两个真实风险：日志写失败可能阻断配置恢复、重入信号可能打断自有sender回收。AST提取实际函数和恢复分支的离线ENOSPC注入观察到RED/GREEN：旧版留下0；新版恢复97但将证据故障记为INCOMPLETE。另验证正常恢复、sender未确认退出时不恢复，共4项通过；这是假相机纯离线测试，不冒充真实故障注入。比较器9项合成自检通过，缺941、来源不一致、B部分通过或恢复记录不全均拒绝。Claude运行前复审及补充安全复审无P0/P1。实机状态文件未绑定控制脚本SHA，不将当前脚本校验或离线测试扩大为实机故障注入证据。

最终 Claude 复审实际返回 `usage_limit_reached`（429，session limit），不是批准。接手后独立只读规范与事实复核均确认上述失败、恢复及未执行边界；远端 TEST 证据目录已通过既有可信 SSH 核实存在。旧 `preparation.sha256` 保留为历史快照，当前脚本应核对 `preparation-final.sha256`，其7项全部匹配。只清理本轮可再生 pycache，补存 `takeover-checkpoint.json` 与两机根 `SHA256SUMS`，未覆盖原始失败。

本次没有修改生产代码、SDK或其他相机配置。A轮ASan+UBSan sender stderr空；B的Go注册/目录期限超时及sender等待授权失败均保留原错误，不称Sanitizer PASS。未重新运行全量桌面/服务端CTest或Go test/vet/race。旧第10节和本节音频门槛均未改为PASS，Qt采集、正式广播控制owner、实际听音及完整对讲仍待完成。
