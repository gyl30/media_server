# 安装版播放授权挂起时退出

2026-10-18，DEV工作树`7691504`；仅仓库外一次性监督，生产实现和已有覆盖不变。此轮填补外部请求屏障下的关闭观察，不是媒体解码或物理输出验收。

## 实际路径与判据

独立loopback27380–27385、fresh SQLite、私有认证Xvfb，从`/`启动既有安装版`run.sh → gb_monitor`。对同一clean d06f0c0包逐项核对236条SHA：安装ELF `8dcf4a71e613f8dcee9aae3dbc1875c0db0b7026b283a647bf2f6ba3461273d8`，manifest `250917ea868a1238fc49cee7f4ffc3ba698c21dbb4f8b25083e2ab91bd2ff505`；不是重新构建当前HEAD的产品。

实际后端、独立推流设备及H.264/AAC发布者首先ready。Qt设备列表来自真实Go API，经私有HTTP代理原样转发；XTest双击真实列表后，代理必须实际收到完整`POST /api/play`并核对当前设备body，才允许发送WM_DELETE。仅这一个请求在代理处挂起，**不向上游转发、不返回授权、不创建观看媒体**。不是固定sleep假设“正在连接”，也不是调用Qt测试信号。

预设要求：唯一请求、关闭前仍挂起；WM_DELETE后Qt5秒内退出0，代理在自身关闭前观察到客户端EOF；从挂起到EOF小于8秒，以排除生产10秒请求超时作为退出原因；私有RTSP端口无ESTABLISHED连接。该端口快照不冒称内部viewer对象精确计数。发布者保留`-nostdin`及stdin=DEVNULL，有限`-t 20`，必须在启动后30秒内自然退出0；不能以TERM或KILL替代自然EOF。

## 结果

监督总20.443秒，退出0，`PASS_PENDING_AUTHORIZATION_CLOSE_ONLY`：

| 事实 | 实际结果 |
| --- | --- |
| 完整播放请求进入屏障 | 5.781308秒；唯一POST、68字节body |
| WM_DELETE发送 | 5.783604秒；安装Qt PID3894948、唯一自有窗口 |
| 代理观察到客户端EOF | 5.801609秒；挂起后20.301毫秒、发关闭后18.005毫秒 |
| Qt退出0 | 5.814948秒；发关闭后31.344毫秒 |
| 上游播放POST | 0；独立核对实际转发清单而非只信初始化计数 |
| 私有RTSP观看连接 | 屏障中及Qt退出后均无ESTABLISHED条目 |
| 发布者 | 自然EOF退出0；监督收到退出在20.413842秒 |
| 清理 | 代理线程/handler结束，设备删除、receivers空；Qt/publisher/Xvfb/Go/media各退出0，无KILL、PID/组无残留，xauth删除、端口可重绑 |

代理未向上游发起播放，因此只能证明**授权前申请尚未完成时关闭**。不扩张为授权后启动中断、GB INVITE迟到、媒体播放中关闭或真实相机动作；这些有独立组件/协议测试，不能由本轮代替。[源替换两轮](qt_monitor_client_stage7_source_cycle.md)的原FAIL_CLEANUP仍保持，后来的真实录像按钮轮关闭失败也须独立调查，不能被本PASS覆盖。

## 证据

DEV `/var/tmp/qt-connect-close-7691504-EsAANECW/`保留完整监督源码、原始result、每个进程PID/starttime/PGID/命令、ELF及fixture SHA、原始日志、转发清单和挂起请求。只有本轮拥有的资源被关闭，未使用其他服务或用户显示。

- `qt_connect_close.py` SHA256：`0b7bf394f2ec68df0d67b5fa662b70bd0a4fcc23482f682bd51610c4c408c0e1`。
- `result.private.json` SHA256：`f474cd9e6e01ba28371bf896e896748d23c89f8824b00490c7a989760037425b`。

原始授权/URL和日志只留私有目录，不提交Git。此轮非ASan程序、无音频/画面完整性结论，不代表七阶段整产品GO。

接手后已重新核对上述脚本/result SHA及完整SHA清单；[安装版录像控制与主动关闭](qt_monitor_client_stage7_installed_controls.md)另补已建立媒体的实机观察，并保留原run01及后续工具失败。该后续结果不改变本轮仅覆盖授权前挂起取消的范围。
