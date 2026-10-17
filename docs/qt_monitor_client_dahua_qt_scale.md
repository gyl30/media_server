# Qt 到真实大华同会话倍率呈现补验

## 1. 范围与身份

本轮以干净的 `d26618c5d94335dc8e93ae9b0995be6d11d78ef8` 为基线，只运行仓库外私有观察探针，未修改 Qt、Go、C++ 生产代码或第三方。接续[受控倍率回归](qt_monitor_client_stage4_scale_rates.md)与[真实定位补验](qt_monitor_client_dahua_qt_seek.md)，验证真实 Qt 播放组件在真实设备输入下请求 Scale2、再恢复 Scale1 的行为，不重新开展容量测试。

- DEV：`/var/tmp/qt-dahua-scale-d26618c-MFTVIMKb/`。
- TEST：`172.20.63.32:/var/tmp/qt-dahua-scale-d26618c-vsAhd2J4/`。
- 摄像机：Dahua `172.20.44.240`，设备 `34020000001320000089`，通道 `34020000001310000089`；既有平台地址不改。
- 新 RecordInfo 查询仅找到一个满足首跑前最低60秒条件的63秒范围：本地 UTC+8 的 `2026-10-17 14:44:25–14:45:28`。优先90秒范围不可得，不拼接空隙、不修改设备录像或编码配置。

TEST 使用新目录中的 SQLite 备份，不修改原数据库；原 SHA256 `f85a2485732c8e99d5df0b2787480705dbca34aac5aa879fb03a51cbb6e0a7e3` 前后相同。既有设备注册和目录在新服务中实际恢复，不用清空状态或重置广播 SourceID 换取成功。

媒体服务器由现有独立 build 对当前源码增量构建，退出0，不声称 fresh build。ELF SHA256 为 `45f0c48affb7b866578178f5298fe1291d6c6efbeab7f79c34366f54dda4fde4`；未改媒体路径的近期 Qt/test 提交不改变该 ELF。当前源码重新构建 Go signaling，ELF 为 `93a4653da40dda228a89482f9b38cc10a6105a6829902157a226841f1d2ddc05`。`runtime-manifest-preserved.json`、`signaling-buildinfo.txt`、构建日志及部署校验记录来源，不从 build 目录名称推导版本。运行包57项文件在 TEST 启动前全部核对，manifest SHA256 为 `59f2a8bff2695015243d48330e5fc61e1ad77b231710dab1ef667b92425656b7`。

## 2. 首跑前固定的观察方法

私有 `scale_probe.cpp` SHA256 为 `efc7361d4ed34df856eef36542c189a5f3a1b2d71dd70543d8c5b332b42f8d50`。实际调用生产 `playback_view::play_record()` 一次，再调用 `scale_record(2)`、`scale_record(1)`；不另开播放器或旁路媒体 GET。Normal、ASan 分别为独立会话，分别核对输入证据。

首次**观察到** `record_seek_ready`、解码PCM超过96000帧且呈现超过25帧时记为 ready。必须在启动15秒内满足；不是声称捕获了精确的 ready 信号时刻。其后预先固定：

| 相对 ready | 操作 / 观察 |
| --- | --- |
| 2–6秒 | 基线窗口 |
| 8秒 | 请求 Scale2 |
| 12–20秒 | 改速后窗口 |
| 20秒 | 请求 Scale1 |
| 24–32秒 | 恢复窗口 |
| 32秒 | 主动停止，不等待自然EOF |

100ms PreciseTimer 执行观察；每个窗口起止、请求发起和停止的调度迟到不得超过250ms。250ms不是 HTTP/SIP 应答期限，也不是媒体生效期限。总观察60秒兜底，停止后对象释放最多15秒，外层执行器90秒。没有根据204或结果重新选择窗口。

每窗必须有实际 Qt render/Paint、解码PCM和真实 SDL 提交进展；检查首次ready采样捕获的同一 owner、唯一 decoder、SDL playing、无队列错误及最终 children/SDL队列清零。请求受理仅是局部门禁，不断言设备必须达到请求值。全部25项断言满足后输出 `WINDOW_COLLECTED`，不以此冒充倍率精度、全库存或自然EOF PASS。

SDL、SWR 包装均先且仅调用一次真实函数，再观察返回和数据；没有伪造输出。GUI SWR 只计GUI线程调用，解码线程的转换不混入。Paint事件不拦截，图像 SHA 最多每100ms取一次；它不是每个实际显示帧的源PTS。`last_video_pts_us` 是解码前沿，不作为 painted PTS。

环境为 Qt6.2.4、GCC16.0.1 experimental、既有 FFmpeg9.0.1 shared SDK，QPA offscreen 和 SDL dummy。运行库目录虽名为 `gcc-16.2`，不据此把实际编译器写成16.2。原 `README.md` 为首跑前设计快照，其中“未编译/运行”只描述该快照；实际结果以下列进程、JSON、映射和抓包为准。

## 3. Qt 真实观察结果

| 项目 | Normal01 | ASan01 |
| --- | ---: | ---: |
| 进程退出码 / 状态 | 0 / WINDOW_COLLECTED | 0 / WINDOW_COLLECTED |
| 局部断言 | 25/25 | 25/25 |
| 首次观察 ready | 7.500秒 | 7.501秒 |
| JSON 总时长 | 40.100秒 | 40.000秒 |
| SDL QueueAudio 调用 / 错误 | 3021 / 0 | 3019 / 0 |
| SDL累计S16交错样本 / 非零样本 | 3380218 / 3332240 | 3380628 / 3332844 |
| 提交后最大观测队列 | 30592字节 | 30592字节 |

队列值是每次提交后观察到的最大值，不是连续监测的绝对峰值。S16为48kHz双声道，PCM frame = 交错样本数 / 2；不混用声道样本与frame。

| 轮次 / 窗口 | 实际相对ready区间 / 秒 | render / Paint | 采样图像变化 | 解码PCM增量 / frame | GUI SWR输入 / 输出frame | SDL交错S16增量 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Normal 基线 | 2.000–6.000 | 106 / 106 | 35 | 201600 | 0 / 0 | 403200 |
| Normal 请求2后 | 12.000–20.000 | 384 / 384 | 70 | 740160 | 737574 / 404440 | 808880 |
| Normal 恢复1后 | 24.000–32.000 | 200 / 200 | 66 | 385920 | 382804 / 404685 | 809370 |
| ASan 基线 | 2.100–6.100 | 106 / 106 | 35 | 201600 | 0 / 0 | 403200 |
| ASan 请求2后 | 12.100–20.099 | 398 / 398 | 72 | 766080 | 765523 / 404938 | 809876 |
| ASan 恢复1后 | 24.100–32.099 | 201 / 201 | 67 | 385920 | 385182 / 404729 | 809458 |

Normal 请求2后 render 为48.00fps，恢复为25.00fps；ASan分别为49.756fps、25.128fps，分母使用实际7.999秒。基线106/4=26.5fps，不把短窗计数强写成设备精确25fps。基线未进入倍率SWR路径，GUI SWR计数0而SDL持续提交是预期的直接路径，不能当作没有音频。

三窗SDL非零样本分别为 Normal `397478 / 797238 / 797336`，ASan `397478 / 798326 / 797540`；环境声音静音也合法，此处只记录观测而不据非零值宣布声学验收。GUI SWR输入/输出反映本地重采样工作，不单独推导设备输入速度。

Normal两次受理分别晚于发起465/467ms，ASan为589/429ms。所有观察窗仍按首次ready固定，不把受理时刻当作速率切换点。

## 4. 独立输入与协议核对

同一份冻结抓包 SHA256 为 `1fa86a330f446e4cd0c634f585cd61693173864e293b0a0eec836715adac8be2`，共122337个包，capture/receive数相同，kernel drop为0。Normal选择摄像机TCP stream7，ASan选择stream13；各自SYN、INVITE SDP地址/端口/PT及应答SSRC与实际RTP匹配。源PES/ffprobe识别H.264和8kHz单声道PCMA，不从历史媒体资料补造本轮codec。

私有离线 `scale_pcap_witness.py` SHA256 为 `c5c84190a81ea035a0046f558f5b0bd73a607cb87ab25f6c9d87f949cf28df7c`。从SYN序号开始逐字节TCP重组，保留每字节首次捕获时间，prefix maximum计入TCP队头阻塞；剥离完整RFC4571/RTP后解析有界MPEG2 PES，以**完整PES已经到达**的时刻对其PTS做OLS。不得把 `ffprobe packet.size` 当PS长度，或把包含该packet的PES到达时刻冒充完整AU可用时刻。

两次Scale的Qt发起/实际受理时刻，与抓包对应SIP INFO/200、嵌入RTSP CSeq/200建立短程时钟偏移因果区间；不用代理结束时间替代Qt收到应答的时刻，不用204作为媒体切换点。两区间交集收缩固定ready窗口边界，Normal/ASan偏移不确定宽度分别3.697/3.209ms；没有校正未知时钟漂移或抓包时间误差。这里的PES窗口与Qt实际Paint统计端点不是逐帧对齐，尤其ASan采样端点晚约100ms，不能据二者短窗计数差推导丢帧。

| 轮次 / 窗口 | 有效输入观察秒数 | 视频PES数 / PTS秒每到达秒 | 音频PES数 / PTS秒每到达秒 |
| --- | ---: | ---: | ---: |
| Normal 基线 | 3.996303 | 101 / 1.000453 | 66 / 0.999503 |
| Normal 请求2后 | 7.996303 | 385 / 1.930920 | 257 / 1.931507 |
| Normal 恢复1后 | 7.996303 | 199 / 0.997752 | 134 / 0.998419 |
| ASan 基线 | 3.996791 | 99 / 0.999194 | 67 / 1.000787 |
| ASan 请求2后 | 7.996791 | 398 / 1.972202 | 264 / 1.971028 |
| ASan 恢复1后 | 7.996791 | 199 / 0.999852 | 134 / 0.999493 |

Normal三窗的OLS视频残差RMS分别为0.020022、0.047534、0.021532个PTS秒，音频为0.011203、0.043407、0.012391；各窗PTS均无倒退。独立只读复算与receipt斜率、样本数及RMS一致（浮点误差小于1e-12）。这些是捕获位置实际到达节奏，包含本轮网络/缓冲/背压影响，不是设备无背压发送上限、精确控制延迟、实际paint源PTS或物理音频速度。

ASan视频RMS为0.023191、0.044436、0.023158个PTS秒，音频为0.014292、0.038558、0.012899，各窗同样无倒退，独立复算同样一致。两轮不是固定输入速率的性能A/B；约1.931/1.972的实际改速差不能归因于ASan开销或生产代码改变。只报告各轮输入与本地实际工作，不承诺设备精确2×。

Normal输入25909222字节PS、22080个完整RTP，TCP无字节缺口/冲突重传，RTP序号无断裂；tshark记录1个乱序、0个重传及0个zero-window。主动停止时留下16字节不完整RFC4571尾和1402字节PS尾，不拿它们做PES回归，也不声称全录像完整。关闭位置有平台FIN及随后RST，保留原记录，不写成自然EOF或全程无RST。

ASan输入26503856字节PS、22561个完整RTP，TCP无字节缺口/冲突重传，RTP序号无断裂，tshark重传/乱序/zero-window均0。该轮主动停止留下58字节RFC4571尾、5566字节PS尾，收尾同样有平台FIN/RST。独立输入回归只用完整PES，不隐去截尾。

API201返回URL在内部与唯一实际媒体GET匹配，Scale/DELETE精确绑定该201的 `playback_id`；每轮各为201、204、204、204。HTTP与SIP没有共享Call-ID字段，因此再结合唯一GET之后的INVITE及SDP/RTP匹配建立关联，不把时序关联包装成跨协议的加密身份绑定。每轮同dialog仅一个逻辑INVITE与ACK，两个Scale事务均获SIP200且嵌入RTSP200；完整头、token与媒体路径只留私有目录，不进入Git。

Normal的两个Scale各一条线上请求；ASan Scale2首次捕获到的应答晚于首次发送500ms，期间重传一次，仍为同CSeq899、Via branch和body的一次逻辑控制，Scale1只有一次请求。Normal BYE同CSeq3529、同branch和空body重传一次，线上2请求/2个200；不是双方各发BYE。ASan BYE901为1请求/1个200。独立review仅用 `editcap` 从原pcap摘10个SIP包，再核对Call-ID哈希、CSeq、方向、Via branch及body；小切片SHA256为 `bac4927e1c877904f04e1d37a991d3bd61ed4e7889ad31567de75aeb50a51f2f`，保留于 DEV `/var/tmp/qt-dahua-scale-sip-audit-WfJbw8Mr/selected.private.pcap`。没有为消除重传日志修改生产超时或重试规则。

两轮离线工具均退出0，状态是 `OFFLINE_OBSERVATION`，不自创设备倍率合格阈值。收据位于 `normal-input-witness-01/receipt.json` 和 `asan-input-witness-01/receipt.json`，SHA256分别为 `7308e2f34bec96cbfb5cd32ebe0e9d1fe1eb89aa148b5d5b9e0020e68d03fb9b`、`47f9571b1e5d91932c951ca6348ecde942452095f5a0cc8fc817c6ba6146c4d3`；PS/PES/RTP映射、ffprobe packet解析及私有TSV均保留。ffprobe只做离线packet解析，不替代本轮真实Qt解码，也不把CLI版本7.1.1混称Qt所用SDK9.0.1。

## 5. Sanitizer 与资源释放

Normal探针 ELF SHA256 为 `9cf8a20127242d34a2e1ef584c58e70244c9425c821477062c96dcc8b3b0ef8e`，ASan探针为 `1c6e3a150e2ef8fc1dfc4bea6c44e27cf187ab669b85ab2e99c4cadb48e1849b`。`probe-inputs.sha256`、两份 preflight、构建参数及运行期 maps 记录相同源与实际链接。

ASan + UBSan 的探针和桌面组件真实插桩，实际加载 `libasan` / `libubsan`；`detect_leaks=1`、`halt_on_error=1`，LSan非零错误退出仍开启，无suppression。进程退出0、stderr为空，未见ASan/UBSan/LSan诊断。TEST后端为Normal，Qt/SDL/FFmpeg等外部shared库不是本轮完整插桩，不能写成整个平台完整ASan PASS。

本轮未改仓库生产代码或既有回归的编译输入；新建并编译了私有观察探针。未重跑完整CTest或Go test/race/vet，不把历史回归计数挪为本轮结果。两个私有Qt真实设备观察不等于新增两项CTest。

停止后两轮各自的decoder/owner均实际释放，SDL队列归零。TEST `receiver-after.json` 为 `{"receivers":[]}`，`cleanup-final.json` 确认 signaling/media 退出0且PID消失、SIP15060与TCP38470–38473可重新绑定、原数据库SHA不变。DEV控制代理按PID/starttime核对后正常退出，不终止其他进程。没有修改系统sysctl、limits或网络配置。

仅清理本轮自建且确认无进程引用的DEV重复 `runtime/`：57项逐项SHA与TEST保留副本一致，manifest另存后删除345731432字节（约329.7MiB）；`owned-cleanup.json` 与 `test-runtime-verification.json` 为核对证据。TEST完整runtime、DEV原build、私有探针、原始日志、抓包和失败证据均保留，不触及其他Agent或用户文件。后续清理同样仅限我们在`/tmp`、`/var/tmp`产生且已无用途的产物。

## 6. 结论与边界

两轮真实Qt→大华的预定同会话Scale2→1观察和资源门禁通过；请求值、协议受理、实际输入及呈现工作分别取证。只更新该设备、该63秒库存范围和本次预设窗口的结论，不宣布完整第四阶段或七阶段GO。

仍未验证真实相机的全部五档、暂停/倍率组合、输入断供恢复、物理显示/扬声器或精确声画同步。SDL dummy并非实测48kHz声卡时钟，Paint并非物理scanout；采样图像变化也不证明每帧或精确源时间。

本次计划主动停止，不验证完整库存逐帧/PCM无损或自然EOF。高倍率完整性、逐帧paint及PCM库存仍依赖另行记录的受控回归，不能迁移为这两轮真实采集的全量结论。PCMA设备结果不外推宽带AAC快放音质；原SWR低通边界不变。

没有新增生产状态、配置、线程、timer、HTTP接口、helper或抽象层；本轮仓库改动仅为证据文档及计划状态更新。
