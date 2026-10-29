# 真实 GB28181 摄像机端到端验收记录（e12340d → 891aec1）

## 环境

| 项 | 值 |
| --- | --- |
| DEV | <DEV_HOST>；Google Chrome 153.0.8010.52（headless，Playwright 驱动，`--use-file-for-fake-audio-capture` 注入预声明音源，`--unsafely-treat-insecure-origin-as-secure` 模拟 HTTPS 部署）；ffmpeg n7.1.1 作 RTSP 观看端 |
| TEST | <TEST_HOST>；TEST 目录 `/var/tmp/gb-camera-e12340d-fBaRgeOC/`（完整抓包 capture-s1.pcap、platform.db 留在该目录） |
| 媒体服务 | build/media_server（HEAD 源码增量构建为最新，sha256 前缀 ef9aab26），`--threads 2`，HTTP 38472 / RTSP 38471 / RTMP 38470，运行库 rt-base/lib |
| 信令 | 静态 Go 构建：e12340d `40e4886d` → 6a87c5a `a7840791` → 891aec1 `6f3e882a`；SIP 15060，HTTP 38473（嵌入式 Web 控制台） |
| 摄像机 | 大华 DH-SD-P3A1205-P，<CAMERA_HOST>（用户给出的 <CAMERA_ALT_HOST> 从 DEV/TEST 均不可达，<CAMERA_HOST> 的型号与凭据核对一致）；H.264 1920×1080 25 fps，PCMA 8 kHz；AudioSource=Mic、输入音量 65、扬声器音量 97 |
| 接入方式 | 摄像机平台槽位 1（sipServer[1]）：设备 34020000001320000001、通道 34020000001310000001、平台 34020000002000000001、域 3402000000、本地端口 5061；槽位 0 全程在线注册在 <OTHER_PLATFORM_HOST>，未改动 |

## 摄像机配置变更与恢复

- 变更前：槽位 1 `serverOption=Disable`、`sipSvrIp=<ORIGINAL_SIP_SERVER>`、`sipSvrPort=5060`（`camera/slot1-original.txt`）。
- 测试期间：槽位 1 改为 GB28181 → <TEST_HOST>:15060；CGI 读出的密码为掩码 `******`，无法得知原值，因此把槽位 1 的密码设为既有测试密码。
- 22:08:47 恢复：`serverOption=Disable`、`sipSvrIp=<ORIGINAL_SIP_SERVER>`、`sipSvrPort=5060`。完整配置前后比对（排除 IsOnLine 与时间类字段）无差异，密码字段是掩码，比对不出变化。
- 恢复后平台按 90 s 心跳超时置离线（22:09:32）。摄像机槽位 1 的 `IsOnLine` 状态仍为 true（变更前为 false），属设备侧状态显示，摄像机已不再向 TEST 发送报文。
- 广播 SourceID 高水位：15 → 18（本轮使用 …360000016/17/18）。后续实验从 18 继续。

## 结果

| 项 | 结果与证据 |
| --- | --- |
| 预检 | 20:57–21:23 共 26 分钟，TEST 抓包 0 个摄像机报文；原因是槽位 0 指向 <OTHER_PLATFORM_HOST> |
| 注册 | REGISTER 401 → Digest 200（21:46:40）；Keepalive 每 60 s；Web 控制台添加设备后 UI 显示在线、心跳 |
| 目录 | 注册后自动 Catalog，返回 1 个通道（IP PTZ Camera，ON），详情页显示“1 个，在线 1 个” |
| 点播信令 | INVITE（UDP，recvonly，y=0200000001）→ 200（摄像机 sendonly 9704）→ ACK；接收端创建到媒体就绪约 1.6 s |
| Chrome 出画 | WHEP H.264 4d001f 1920×1080 25 fps，211 帧解码 / 5 关键帧 / 0 丢包；PCMA 8 kHz 音轨接收与解码样本持续增长；状态：正在请求播放 0.1 s → 正在连接 0.2 s → 等待画面 2.3 s → 播放中 3.8 s |
| RTSP 观看端 | ffmpeg 20 s：H.264 1920×1080 500 帧解码，PCMA 8 kHz 单声道，RMS −62.6 dB（环境安静） |
| 并发观看 | 同一 live 同时 2 个 WHEP + 1 个 RTSP；关闭监控页画面后该 WHEP 会话关闭，详情页继续解码（3475 → 3590 帧），live 保持 |
| 对讲信令 | Broadcast Notify（SourceID …360000016）→ 摄像机 Result=OK → 摄像机 INVITE（audio 9708，PCMA，recvonly）→ 平台 200（sendonly 49156）；UI：正在连接约 1.5 s → 正在上送麦克风音频 |
| 对讲上行 | WebSocket 上传（TCP 38472）→ RTP PT8：1563 包 × 160 字节，31.19 s，序号无缺口，时间戳步进 160，SSRC 270623850 = SDP y；解码后预声明五频音序依次检出（连续 51 段） |
| 设备实际播放 | 同时录制摄像机回传音轨：对讲期间五频能量较对讲前提升 69.5 dB（RMS 14.5 → 3275，对讲结束后回到 15.9），顺序与源一致，满足预声明判据；非基频能量 −12.3 dB（上行 −38.8 dB），音调结束后无 40 dB 快速衰减，符合扬声器到麦克风的声学路径，非数字直通 |
| 停止对讲 | UI 停止 → BYE 200，talk 404，音频端口释放 |
| 对讲占用 | 第二个页面开始对讲显示“设备正在对讲中”，原对讲不受影响 |
| 停止点播 | 确认框默认焦点“取消”；确认后 BYE 200，摄像机 RTP 停止，receiver 删除，WHEP 结束，所有播放器显示已结束；重试后新 live 出画 |
| 设备注销 | 槽位 1 停用 → REGISTER Expires 0 → 平台离线，向摄像机发 BYE（无响应，3 s 超时），receiver 释放；注销的 200 在 BYE 超时后才发出（摄像机重传一次） |
| 注销时播放器 | 修复前：离线后仍显示“播放中”约 19 s，随后误报“连接中断”；修复后（6a87c5a）离线同时显示已结束 |
| 恢复 | 槽位 1 重新启用后 3.9 s 上线、目录恢复、重新点播出画 |
| 异常断网 | TEST 临时 nft 丢弃摄像机入站（22:02:21–22:04:18，已删除）：20 s 后 receiver 空闲超时，两个页面播放器同时显示已结束，live 经 BYE 超时清除；90 s 心跳超时离线（22:03:15），对讲随之结束，平台侧 talk、receiver、音频端口全部释放；网络恢复后 90 s 摄像机重新注册，两个页面重试出画 |
| 信令重启 | 平台重启后摄像机心跳被 403，摄像机按 3×60 s 判定后重新注册，约 3 分 40 s 恢复；旧 receiver 被清理，摄像机停止发送 |
| 资源 | 每次停止或清理后 `/receivers` 为空、媒体进程 UDP 套接字 0；结束时端口 15060、38470–38473 可用，进程退出 |

## 观察（未修改）

- 摄像机每次槽位启用后的首个 Keepalive 携带 `SN=0`，平台对所有 MANSCDP 要求 SN > 0（`heartbeat.go:41`），返回 400；后续心跳正常，不影响在线。标准原文本次不可得，未改代码。
- 摄像机 PS 音频 PTS 步进不规则（40/80 ms 交替，40 次重复），RTSP 输出中的重复 DTS 警告来源于设备。
- 密码错误期间摄像机以最高约 12 次/s 重试 REGISTER，平台逐个 401，平台无异常。
- 点播刚就绪时通道徽标“等待媒体”比播放器“播放中”晚约 3 s 更新（2.5 s 轮询）；注册后数秒内概览“国标通道”为 0，目录到达后为 1。
- 网络中断导致对讲结束时，UI 提示“音频上传连接已断开；对讲清理未确认，平台将按期限回收”，平台实际已释放（DELETE 因 BYE 超时返回 502）。
- 自己停止点播后，本页播放器显示带警告图标的“媒体已结束”与“重试”。
