# 单一SIP平台的广播音量对照

## 范围

2026-10-17，生产Go基线53b0e4d（广播实现与347decb相同）。仓库外私有overlay调用生产内部 `startBroadcast/stopBroadcast`，DEV独立PCMA/RTP发送，TEST私有UDP收集器通过生产实时SIP点播采集相机PS。不是Qt麦克风、生产C++接收器、广播公开API或物理听音。

三轮共用一个真实注册/目录就绪的SIP进程，A97→B0→A97，不逐轮重启、不假在线、不改注册规则。每轮相同8秒五段音、peak512，DEV发送400个RTP/64000源样本；原发送器SHA `373f607a3092ab01f7d2d7dd971d3f8b38c7930640a957102efb4f3939147cb8`。生产owner确认Notify2xx、独立Result=OK和反向INVITE真实ACK后才授权发送；每轮6秒baseline及6秒post独立保留。

原分析器SHA `0c36fa48b2e1bf7d304451a53906d7cc10f474794d409af9547b99e09193b03b`、比较器SHA `539511d2a2a3ec975527fce3962609d6a4d83327d44852185dfef41c15893060`逐字节复用。未改PTS3ms、五频、相关性或10dB对照门槛，不覆盖[旧音量实验](qt_monitor_client_dahua_broadcast.md)的INCOMPLETE。

## 首轮工具失败与修正

首轮DEV `/var/tmp/qt-broadcast-stable-347decb-80YeLWgr`，TEST `/var/tmp/qt-broadcast-stable-347decb-KZaJibxo`。实际三个Go子测试均正常收尾，SourceID尾号6/7/8；外层末轮先查完成标记未出现，再查到平台已正常退出，误判为未完成。原 `experiment-status.json` 保持 `INCOMPLETE`、只收录前两轮；finally确认恢复97，平台退出0，第三轮原始资产另行补收，不重写原结果。

私有监督循环用真实AST建立竞态RED，再最小修正为平台正常退出时重读完成标记，GREEN。另有四项纯AST恢复测试，覆盖日志ENOSPC、正常恢复、发送器未确认、外部音量改变；清理阶段日志失败不阻止恢复，但结果必须INCOMPLETE。保留旧脚本、原失败、预声明和全部媒体。

## 修正后独立复验

新DEV `/var/tmp/qt-broadcast-stable-53b0e4d-8VlRA8zg`，TEST `/var/tmp/qt-broadcast-stable-53b0e4d-roAoReyl`；Go ELF SHA `7944291b543dac40de5fd39df8b6c0d8b223ad0e89a7927fae85627d8d868c71`。三个case的协议、原始媒体、配置读写事件和正常结束完整，`COMPLETE_PROTOCOL_ONLY`；原比较器核实音量来源、顺序及恢复为true，音频结论仍为 **INCONCLUSIVE**。

| 记录 | A-before97 | B-muted0 | A-after97 |
| --- | --- | --- | --- |
| 相机RTP包 | 10396 | 10548 | 10381 |
| PS字节 | 12112836 | 12296577 | 12117888 |
| H.264解码帧 | 583 | 600 | 572 |
| PCMA解码样本 | 186720 | 192480 | 183360 |
| PTS最大局部误差 | 60ms | 60ms | 70ms |
| 非验收样本域整段相关 | 0.606794 | 0.155644 | 0.636296 |

三轮原始RTP序号均无观测缺口、唯一SSRC，payload拼接与保存PS逐字节一致，音频帧样本数与解码PCM一致；这些只覆盖相机→TEST采集，不证明DEV→相机收包。三份源PCM/ALAW的SHA完全相同。B和A-after的解码警告保留，工具退出0不替代声音验收。

三个严格分析均因音频PTS前提不满足而INCONCLUSIVE。非验收样本域诊断中，两个A仅1877/1511Hz两个频带通过，B五频均未通过；完整五频及A-B的10dB对照未全部满足。部分频带随音量变化是本轮观测，不能扩大为完整声音通过，也不足以断言源IP拒收、AEC、具体失真或背景成因。

## 清理与边界

相机仅改 `AudioOutputVolume[0]` 的0/97；每轮前后GET，SET后立即GET，发送器确认退出且端口可重绑后才切音量。最终音量97，`restored=true`；平台PID2213223退出0、live/broadcast owner归零，DEV三个sender均已回收，DEV38442/43及TEST15060/38440/41可重新bind。凭据只从交接指定0600文件进内存，未入源码、日志或argv。

新DB由已退出的上轮DB只读SQLite backup得到，保留高水位8，本轮使用9/10/11；两个旧DB SHA不变。后续广播须以新根高水位11继续，不能回退旧副本；复制/回滚/重建数据库的跨库唯一性限制仍存在。

本轮只修改私有取证工具和交付文档，没有修改生产媒体路线。旧分析和失败保留，最终比较仍INCONCLUSIVE，不采纳Qt直送或中继方案，不标记广播/对讲、Qt采集/UI或整个平台完成。物理麦克风与听音条件尚待确认；同会话seek/倍速和公共分发等独立缺口不变。
