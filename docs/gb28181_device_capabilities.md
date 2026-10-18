# GB28181 能力补齐与真实大华验收矩阵

> 本文是 `aa4f3f8` 的历史调查快照，以下“未实现/待验证”不代表当前代码。2026-10-18 当前已实现能力、尚未实现功能、兼容限制与明确延期统一见[后续清单](qt_monitor_client_followups.md)；seek、Scale、内部 Broadcast 及 Qt/Web 对讲已经有后续实现和限定验证，见[七阶段收口表](qt_monitor_client_stage7_installed_controls.md#6-七阶段与审计收口)。原始失败和当时的凭据阻塞保留，不倒写历史结果。

## 目标与标准边界

2026-10-16 用户确认继续使用既有大华摄像机测试 GB28181 功能，标准定义的能力尽可能支持。以真实需求、标准和设备证据逐项实现，不采用 Dahua 专用协议层，不把“尽可能支持”写成已经通过全标准认证。

[国家标准官方公开页](https://openstd.samr.gov.cn/bzgk/std/newGbInfo?hcno=8BBC2475624A6C31DC34A28052B3923D)当前将 **GB/T 28181—2022** 列为现行，自 2023-07-01 实施，全部代替 2016 版。当前 Qt 分支的增量实现主要依据 2016 版，不能据此直接宣称 2022 全兼容。保留已有设备互通，后续单列新版差异、必选/可选及角色适用范围；未读取新版正文的条款不补造结论。

需要始终区分：标准定义、平台实现、设备支持、受控协议测试、真实设备验收。某个摄像机不具备云台/存储/扬声器，不等于平台永远不支持该项；一次超时也不能直接归类为设备不支持。

代码核对 HEAD `aa4f3f85107b69e3629c230252fa7d9ad6aa7593`，工作区 `/home/gyl/example/media_server_qt`，分支 `feature/qt-monitor-client`。下表的“已实现”是代码事实，不自动继承历史测试为该 HEAD 的真实设备 PASS。

## 真实参考设备

| 项目 | 已知配置 |
| --- | --- |
| 厂商、IP | Dahua，`172.20.44.240` |
| GB 设备编号 | `34020000001320000089` |
| GB 通道编号 | `34020000001310000089` |
| 摄像机 SIP 端口 | `5065` |
| 保留的测试平台 | `172.20.63.32:15060` |
| 平台编号 / 域 | `34020000002000001270` / `3402000000` |
| 历史真实媒体 | H.264 Main、1920×1080、25fps，PCMA 8000Hz 单声道 |

历史已完成注册/认证/心跳/目录、点播/ACK/RTP-PS/BYE、RTSP 与 Chrome WHEP 双轨解码、多 viewer 共享上游和人工音画验收；这些是历史范围，不证明当前所有新增查询、控制或下载功能已在该设备通过。

2026-10-16 14:54（DEV 时区）只读预检：DEV 到摄像机 ICMP 2/2 响应，HTTP 80 HEAD 返回 200。只说明网络与 Web 服务可达，尚未证明当前 GB 注册、音频开关、录像或扬声器能力。未尝试默认密码、未修改摄像机配置。

本轮 TEST 预检时 SIP UDP 15060 与建议的 38300–38303 端口未被占用，没有现有 media/signaling 服务；其他 Agent 保留未动。媒体端口池实际固定为 49152–65535，不支持 CLI 任意改区间，不能宣称整池独占。原 SIP 0600 凭据文件未在 DEV/TEST 已检查的项目资料、相关临时目录与凭据目录中找到；未断言它已删除，也未扫描无关私人数据。已请求用户提供所在主机与路径。凭据确认前不猜口令、不启动会导致虚假注册失败的测试平台，当前实机 GB 功能验收为 **BLOCKED**，并非设备功能 FAIL。证据与后续日志使用独立 DEV 目录 `/var/tmp/qt-dahua-aa4f3f8-tbkLnwXM/`。

## 当前能力矩阵

| 功能 | 平台当前实现 | 本轮真实设备状态 / 仍需验证 |
| --- | --- | --- |
| REGISTER、Digest、刷新、注销 | 已实现，SIP UDP、白名单及来源/期限校验 | 历史通过；本轮需恢复独立平台并重新验证 |
| Keepalive、离线清理 | 已实现 | 历史通过；本轮不能把 ICMP 当心跳 |
| Catalog 查询与分批聚合 | 已实现，注册后自动及显式查询 | 核验真实完整目录、通道身份；HTTP 202 仅为受理 |
| DeviceInfo | 已实现，名称/厂商/型号/固件/通道数 | 待获取真实型号，不能凭 Dahua 厂商推断 PTZ/音频能力 |
| DeviceStatus | 已实现，自报在线/状态/时间/编码/录像 | 待实测；标准 Alarmstatus 列表尚未对外提供 |
| 实时视音频点播 | 已实现，当前 Go SDP 为 UDP PS/RTP | 历史双轨通过；本轮要用新 Qt 实际解码 |
| 多 viewer、源退出与释放 | 已实现，共享一个上游 live | 新 Qt 受控/实际媒体测试已有；真实相机需复验 |
| 视频 RTP TCP active/passive | 媒体收发组件已支持 | 不等于 Go 实时点播已开放三种 SIP 协商；分别核对 |
| 八方向 PTZ、变倍、STOP | 已实现有界点动及独立 STOP | 尚未确认该相机具有可动云台；SIP 200 非机械动作证据 |
| PresetQuery | 已实现只读查询 | 待实测；空列表本身是合法结果 |
| 预置位设置/调用/删除 | 已实现，控制入口明确区分 | 设置/删除改变配置，调用可能移动镜头；不列入第一批只读探测 |
| 聚焦、光圈、巡航、看守位、拉框控制 | 未完整实现 | 后续按真实能力和标准逐项补齐，不用空按钮代替 |
| RecordInfo | 已实现，有界多批目录 | 待确认 SD 卡/录像；没有录像属于有效结果，不修改录像计划造条件 |
| 独立历史回放与暂停/继续 | 已实现有限 TCP PS 会话 | 受控真媒体已通过；物理录像待验证 |
| 同会话 seek、Scale 倍速 | 未实现 | 先解决 RTP-Info/媒体边界/Qt 解码时钟，不靠重开会话或猜 PTS |
| 历史下载 | 已实现，独立会话，speed 1/2/4 | 设备兼容与实际文件待验证；下载 speed 不等于回放 Scale |
| 本地录制 | Qt 已实现 Matroska 流复制 | 平台客户端能力，不把它称为标准设备录像控制 |
| 主动 Alarm MESSAGE 与独立响应 | 已实现，近期缓存及 Qt 展示 | 受控 SIP/Qt 组合通过；物理事件待验证，不主动布撤防造报警 |
| 报警查询、报警订阅/通知 | 未实现 | 不把接收 MESSAGE 当支持 SUBSCRIBE/NOTIFY |
| Catalog 订阅/增量通知 | 未实现 | 不以定时查询冒充标准订阅 |
| MobilePosition 查询/订阅 | 未实现 | 固定摄像机不一定具备定位能力，需合适的受控/真实设备 |
| ConfigDownload 配置查询 | 未实现 | 优先补最窄只读类型，避免混为 DeviceConfig 写配置 |
| DeviceConfig、RecordCmd、GuardCmd、AlarmCmd、TeleBoot | 未实现 | 会改配置/录像/安防状态或重启，不纳入无确认的真实探测 |
| 广播 | 未实现 | 需要业务响应、设备反向音频 INVITE、PCMA 媒体和实际扬声器；现有视频 sender 不能替代 |
| 对讲 | 未实现 | 可组合既有实时音频“听”与广播“说”；不另造通用双向媒体框架 |
| 上下级联、跨平台资源共享 | 当前不属于已完成 Qt 七阶段交付 | 标准角色与平台对接另行梳理，单摄像机无法验收平台级联 |
| 2022 版新增/变更要求 | 尚未完成完整差异审查 | 明确待核对，不依据旧设备测试宣布全版本符合 |

## 现有入口与实现依据

路由以 `signaling/infrastructure_http.go` 为准：

```text
GET  /api/devices
GET  /api/devices/{d}
GET  /api/devices/{d}/channels
POST /api/devices/{d}/catalog
POST /api/devices/{d}/query                     {"command":"DeviceInfo" 或 "DeviceStatus"}
POST /api/devices/{d}/channels/{c}/presets/query
POST /api/devices/{d}/channels/{c}/records/query
POST /api/devices/{d}/channels/{c}/ptz
POST /api/devices/{d}/channels/{c}/play
POST /api/devices/{d}/channels/{c}/playback
POST /api/devices/{d}/channels/{c}/download
POST /api/playbacks/{id}/pause
POST /api/playbacks/{id}/resume
DELETE /api/playbacks/{id}
DELETE /api/lives/{id}
GET  /api/alarms/since/{cursor}
```

查询的 SN/当前注册来源/期限核验见 `signaling/device_query.go`；目录聚合见 `catalog.go`、`channel_registry.go`；PTZ 指令白名单与独立 STOP 见 `gb_ptz.go`；广播反向 INVITE、SUBSCRIBE/NOTIFY 没有生产 handler，不能通过接口名推断已支持。`DeviceStatus` 的自报字段与本地注册事实分别展示，不新增第二份在线权威状态。

独立有限回放、下载和 INFO 的当前边界见 `gb_playback.go`、`sdp.go`、`media/gb28181/gb28181_playback_session.cpp` 及 [Qt 接手验收](qt_monitor_client_takeover_verification.md)。

另发现需定向核实的规范差异：2016 §9.10.3 要求注册最终 200 的 Date 为 XML 时间，§9.10.1 指定北京时间；当前 `signaling/register.go` 使用 UTC `http.TimeFormat`。这是文本与实现差异，尚未证明导致大华故障。保留差异，不在能力探测中盲改设备时间或把旧互通成功当符合性证明。

## 安全执行顺序与补齐优先级

1. **恢复真实设备基线**：核对 TEST 上既有进程、15060、媒体端口和仓库外 0600 凭据；创建独立目录、数据库及可识别进程。保持摄像机当前平台地址，不猜密码，不挪用其他 Agent 的服务。
2. **先只读查询**：REGISTER/Keepalive → Catalog → DeviceInfo/Status → PresetQuery → 按设备时间查询短范围 RecordInfo。记录真实响应、错误或超时，不能将超时直接记为不支持。
3. **已有功能实机验收**：实时 Qt 双轨、多 viewer、关闭；确有录像时小范围回放/暂停/下载。确认硬件与动作边界后才做 PTZ/预置位/广播或报警触发。
4. **高价值标准缺口**：优先只读配置/能力查询、录像 seek/倍速的可靠媒体边界、报警与目录订阅，以及广播/对讲的窄音频链路。每项先受控 RED/GREEN、生命周期与 sanitizer，再真实设备验证。
5. **其他标准角色和可选能力**：定位、平台级联等以具体角色和设备条件分批安排；不得为补表引入通用 SIP 脚本、插件工厂、第二注册表或无界重试。强制关键帧等涉及既定媒体架构的功能需明确新的需求和代价，不仅因标准列举就恢复已经删除的反向 viewer 控制。

标准范围不等于破坏性操作授权：不自动重启/复位/格式化、不改摄像机 IP、管理员密码、主编码或平台地址，不改变录像计划、布撤防或原预置位。确需临时音频开关时先记录原值、只做必要调整并恢复，不恢复旧 GB 平台。

每项结果必须标明 CODE/CONTROLLED/REAL_DEVICE 证据等级和 PASS/FAIL/BLOCKED/NOT_RUN，不以受控 peer 或本地测试替代物理设备结果。设备不具备某能力时，保留平台实现与受控验证，并明确后续需要其他硬件，而不是放宽验收或伪造成功。
