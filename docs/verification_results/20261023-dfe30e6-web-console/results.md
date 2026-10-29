# Web 控制台验收记录（dfe30e6 → e12340d）

环境：本机 DUT media_server（127.0.0.1）+ signaling（39090/35060），上游 media_server（127.0.0.2）+ ffmpeg 发布；
GB 模拟器 34020000001320000001；浏览器为 headless Google Chrome（drive.py，--webroot 指向工作区 web）。

| 链路 | 结果 |
| --- | --- |
| RTSP/RTMP/WHEP 拉流 | 均达到 media_ready 并实际解码（1280 宽） |
| 上游断流 | 约 2.5 s 内三路均显示连接中断；上游未恢复时重新连接回到连接中断；恢复后重新连接→正在连接→拉流中→出画 |
| RTMP/RTSP/WHIP 推流 | publishing + media_ready；UI 新增 RTMP 推流：待接入→等待媒体→推流中（约 5 s）→出画；停止推流后 publisher 被断开、状态未推流；删除后 API 无记录 |
| GB 注销/恢复 | 模拟器正常退出后立即离线，重启约 2 s 在线、通道恢复，重试出画 |
| GB 异常掉线 | SIGKILL 后约 30 s 媒体结束（已结束），90 s 心跳超时离线；恢复后重试出画 |
| GB 停止点播 | 确认后 receiver 释放 |
| 对讲 | 模拟器不支持 Broadcast：服务端 502 talk_start_failed，UI 显示可读失败文案 |
| 播放生命周期 | WHEP 会话：四画面 3→单画面 1→离开 0→详情 1→离开 0 |
| 设备增删改 | UI 新增拉流约 7 s 拉流中；删除后 /receivers 无对应 stream；改名保存触发重新拉流并恢复 |
| 嵌入构建 | build-go/signaling 提供的 16 个静态文件与工作区逐字节一致；空库页面无控制台错误；服务停止后顶栏显示连接中断 |

未覆盖：真实大华摄像机（<CAMERA_HOST>）未向测试平台注册，真实设备注册/点播/对讲未验证；对讲上行音频未验证。
已知限制：浏览器 WHIP 晚加入观看者需等待下一个关键帧（无 PLI 转发）。
