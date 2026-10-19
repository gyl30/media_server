# Web 管理界面

signaling 通过 Go `embed` 提供 `signaling/web/` 下的静态页面（HTML、CSS、原生 ES module、SVG），没有构建步骤和外部依赖。视觉与交互依据 V3 原型 `gb28181_console_prototype_v3.html`（深蓝侧边栏、蓝色主色、紧凑表格）。

## 页面

| 路由 | 页面 | 数据来源 |
| --- | --- | --- |
| `#overview` | 控制台：接入对象、在线国标设备、国标通道、运行中视频源、接入方式、需要关注、正在运行的视频 | 快照汇总 + 打开页面时取一次在线设备通道 |
| `#devices` | 设备管理：统一设备列表；搜索、协议 / 状态筛选、添加、编辑、启停、删除 | 快照汇总 |
| `#devices/<gb\|rtsp\|push>/<id>` | 设备详情：接入信息、GB 通道与点播、本页独立 WHEP 播放 | 快照 + 该设备通道 + `player.js` |
| `#monitor` | 实时监控：统一设备列表 + 单画面 / 四分屏 WHEP 播放 | 快照 + 展开设备的通道 + `whep.js` |
| `#resources` | 视频资源：可证明正在运行的视频源 | 快照汇总 + 打开页面时取一次在线设备通道 |
| `#status` | 系统状态：接口可达性、数据时间、接入统计；资源指标标为暂不支持 | 快照汇总 |

快照（`store.js`）每 2.5 秒并行请求 `GET /api/devices`、`/api/sources`、`/api/push-devices`；新的刷新中止旧刷新，快照时间取请求发出时刻。GB 通道按需请求，不随轮询逐台刷新：

- 设备列表只为尚无缓存的在线设备取一次通道数；
- 设备详情、实时监控中展开或正在播放的 GB 设备随轮询同步（缓存早于最近快照才重新请求）；
- 控制台、视频资源、系统状态在打开页面时为在线设备取一次，并显示通道数据时间；
- 右上角"刷新"同时更新已缓存的通道。

## 统一设备对象

前端把三类资源整理为同一展示对象（`unifiedDevices()`），不新增服务端统一模型：

| 字段 | GB28181 | 主动拉流 | 推流设备 |
| --- | --- | --- | --- |
| key | `gb:<device_id>` | `rtsp:<source_id>` | `push:<device_id>` |
| 协议 | GB28181 | RTSP / RTMP / WHEP（由 URL 协议确定） | RTMP / RTSP / WHIP（创建时必须指定） |
| 接入方式 | SIP 注册 | 拉流 | 推流 |
| 地址 / 编号 | 20 位设备编号 | 上游 URL | 所选协议的接入地址 |
| 状态依据 | `online`、`last_seen`（注册与心跳） | `desired_state` 是配置，`session.state` 是运行事实 | `state`：pending / used / expired / publishing |
| 操作 | 详情、通道、删除 | 详情、编辑、失败后重试、停止拉流、播放、删除 | 详情、编辑、播放、删除；详情中重新生成地址、停止推流 |

状态含义彼此独立：GB 在线只说明注册与心跳有效；拉流 `created` 表示已创建接收会话，`media_ready` 才表示媒体已注册；推流 `publishing` 只表示接入地址已通过认证。实际音视频是否正常仍以播放为准。每个画面独占播放器、video、PeerConnection 与状态回调，详情和监控复用 `player.js`，不另存槽位连接状态。

## 接口映射

| 操作 | 接口 |
| --- | --- |
| GB28181 添加 / 删除 | `POST /api/devices`、`DELETE /api/devices/{id}`（无修改接口，界面不提供编辑） |
| GB 点播 / 停止点播 | `POST /api/devices/{id}/channels/{ch}/play`、`DELETE /api/lives/{live_id}` |
| 主动源增删改、重试 / 停止拉流 | `/api/sources*`；创建即自动接入。RTSP 用独立用户名 / 密码；WHEP 用独立 Bearer Token，密码字段承载且不回显；RTMP 不支持这些凭据字段 |
| 推流设备增删改（含协议）、重新生成地址、停止推流 | `POST /api/push-devices/credentials`（添加前预生成）、`/api/push-devices*`（创建时必须指定 `protocol`：`rtmp` / `rtsp` / `whip`，并提交预生成的 `token`）、`POST …/publish`（重新生成）、`POST …/stop` |
| 播放地址 | `POST /api/play`（`live_id` / `source_id` / `push_device_id`） |
| 浏览器播放 | `player.js` 复用 `whep.js` 的 `WHEPPreview`，每个画面一个实例 |

## 当前语义（界面如实呈现）

- 播放授权仍为 60 秒、一次性消费：API 签发的五个协议 URL 共用一张票据，任一消费后其他地址失效。UI 不展示五地址或“签发地址”，只在用户点击播放时申请并建立 WHEP。主动源 / 推流设备未 `media_ready` 时返回 `409 media_not_ready`；GB 点播保留先签票据、后接入的既有时序。授权成功后的失败不恢复 token。
- 推流设备接入地址：添加页面选择协议后由服务端预生成凭据并展示真实地址；该凭据只在内存中等待确认，发布认证只认数据库中的凭据，因此确认前不能使用，取消添加不产生设备和有效授权。确认添加（或在详情中重新生成）后凭据写入数据库，5 分钟内可首次使用；首次发布认证在数据库中原子标记已使用，并发认证只有一次成功，重启后仍不可复用；已建立的推流不受期限影响。推流中不能重新生成（不隐式断开现有发布）。地址由凭据和媒体服务器地址重建，详情页随时可查看。
- 推流协议由服务端持久化并校验，创建时必须指定。三种推流地址共用一个凭据，界面只展示所选协议的地址；服务端不按协议限制发布认证。
- signaling 重启不恢复推流运行：媒体侧遗留的推流接收会话按对账删除，设备显示"地址已使用"。
- RTSP、RTMP、WHEP 源创建后自动连接。失败保留配置及运行意图，不无限重连；用户可修改配置或重试。服务启动恢复 `desired_state=running` 的配置，生成新的媒体代，不恢复旧会话。外部一次性上游 URL 已消费后必须由用户提供新地址，平台不替上游签发授权。
- 详情打开不自动播放。GB 选择通道后点击播放按需点播；主动源 / 被动设备就绪后可直接本页播放。切通道、关闭画面、离页和 `pagehide` 只释放该 viewer，不停止共享上游。迟到的请求结果不能重新挂回旧路由。
- GB 预览总以设备与通道请求点播接口，由后端固定当前共享 live；不把列表缓存中的旧 `live_id` 当成新的播放目标，避免显式停止后立即点击时误用旧代。
- 实时监控：预览放入选中画面，已有视频时替换并关闭原画面的 viewer；关闭画面不停止上游点播或拉流；离开页面或 `pagehide` 关闭全部画面；不自动播放，失败不自动重连。

## 主动拉流边界

- RTSP：TCP interleaved 拉流；RTSP UDP 发布不代表支持 UDP 拉流或播放。
- RTMP：真实 Play 客户端握手、connect / createStream / play，经既有 RTMP 输入组件处理 H.264 / AAC。上游拒绝、结束或空闲超时关闭接收会话。
- WHEP：仅 `http://`，固定 H.264 + 可选 Opus，直接可达的 UDP candidate、BUNDLE / RTCP mux、完整 SDP 应答。按 setup 选择 DTLS 角色，验证证书 fingerprint，接收 SRTP 后由既有输入组件把 Opus 转为 AAC；平台向浏览器输出时再采用原 WHEP egress。
- WHEP 客户端做有限 controlling / 积极提名检查，支持本轮验证的直连 ICE-Lite 上游。不承诺任意 full-ICE 角色冲突切换、HTTPS、TURN、Trickle ICE、ICE restart 或 NACK / RTX。STUN 完整事务与端点匹配，consent 独立于媒体空闲计时，到期关闭。
- 已取得 Location 的外部 WHEP 资源在关闭时 DELETE；owner io 在停机时继续排空该清理。HTTP 有 5 秒请求期限，但系统已进入的阻塞 DNS 解析不能保证严格 5 秒内回收。尚未取得 Location 时也无法保证找到远端资源进行删除。

## 不属于本轮的功能

1. 每协议独立的一次性播放 token（当前五个 URL 共用一个）。
2. 推流协议的服务端强制约束：当前协议字段只用于展示，发布 token 不绑定协议，需要单独设计决策。
3. 推流设备的长期接入凭据（当前为一次性地址，每次接入前重新生成）。
4. GB28181 设备改名（`PATCH /api/devices/{id}`）。
5. GB 注册地址、传输方式、注册时间（区分"未注册"与"离线"）。
6. 只读统计：CPU、内存、连接数、带宽、播放会话数、各媒体流轨道与码率、观看数、服务运行时间。
7. 可选：设备 / 通道汇总接口，替代逐设备请求通道列表。

## 浏览器测试

`tests/web_console/` 使用 Node 22 内置 WebSocket 通过 Chrome DevTools 协议驱动 `google-chrome --headless`，`bidi.mjs` 通过 WebDriver BiDi 驱动 Firefox。媒体验收针对真实 signaling、媒体服务器与原生 PeerConnection；`channels_failure.mjs` 单独注入通道接口失败，验证前端不会高频循环请求，不能把该故障注入称为真实媒体通过。

```
WEB_CONSOLE_URL=http://127.0.0.1:29090/ WEB_CONSOLE_DB=<signaling.db> \
WEB_CONSOLE_CDP_PORT=49333 WEB_CONSOLE_SHOTS=<本轮独立截图目录> \
PUSH_DEVICE=<正在推流的推流设备 ID> MEDIA_LOG=<media_server 日志> \
node tests/web_console/cdp.mjs tests/web_console/<脚本>.mjs
```

前置条件：GB28181 模拟器注册设备 `34020000001320000101`（通道 `34020000001310000001`，可带 PCMA 音频），一台推流设备正在用 ffmpeg 推 RTMP（H.264 + AAC）。截图写入 `tests/web_console/shots/`（不纳入版本库）。

CDP 驱动仅连接本次启动的 Chrome 在 stderr 输出的 DevTools 地址，不查询既存端口来发现浏览器。默认使用系统分配的调试端口（`WEB_CONSOLE_CDP_PORT=0`），也可像示例一样显式指定独立端口；端口冲突不能接管已有浏览器。驱动仅清理自己启动的 Chrome；启动失败也执行清理。需要自行隔离媒体服务、数据库和端口。原始日志 / URL 可能含凭据，不公开提交。七输入验收还需独立上游及真实 RTMP / RTSP / WHIP 发布者，不能仅用单路 fixture 的结果代替全部协议通过。

不需要媒体服务的归属回归：`node tests/web_console/cdp_ownership_test.mjs`。它只启动自有假 Chrome 和 HTTP/CDP 哨兵，验证已有端口不会被访问，不代表浏览器媒体功能通过。

## 已知问题

- `WHEPPreview` 关闭时同时发送 DELETE 与关闭 PeerConnection，服务端常先因 DTLS 关闭释放会话，DELETE 得到 404（`whep.js` 视为已释放，资源已回收）。
- 无头 Firefox 的全新配置没有 OpenH264，WHEP offer 不含 H.264，服务端返回 400；界面显示"浏览器与服务器无法协商音视频编码"。
- 原首音频 PTS 回退失败仍保留，不能唯一还原旧连接的逐包原因；同类公共起点前迟到音频的初始化倒退已稳定复现并最小修复，新 ELF 七输入严格双轨通过。真实关页 DELETE 也已用独立 HTTP 观察器确认，整合结论和验证边界见 [接续验收](reviews/web_console_acceptance.md)。
- 1150px 以下设备表隐藏“接入方式”列；手机宽度改用纵向设备卡片，长地址在卡片内截断，不造成页面横向溢出。
