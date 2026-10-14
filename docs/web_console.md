# Web 管理界面

signaling 通过 Go `embed` 提供 `signaling/web/` 下的静态页面（HTML、CSS、原生 ES module、SVG），没有构建步骤和外部依赖。视觉与交互依据 V3 原型 `gb28181_console_prototype_v3.html`（深蓝侧边栏、蓝色主色、紧凑表格）。

## 页面

| 路由 | 页面 | 数据来源 |
| --- | --- | --- |
| `#overview` | 控制台：接入对象、在线国标设备、国标通道、运行中视频源、接入方式、需要关注、正在运行的视频 | 快照汇总 + 打开页面时取一次在线设备通道 |
| `#devices` | 设备管理：统一设备列表；搜索、协议 / 状态筛选、添加、编辑、启停、删除 | 快照汇总 |
| `#devices/<gb\|rtsp\|push>/<id>` | 设备详情：按类型展示信息与操作、GB 通道与点播、播放地址 | 快照 + 该设备通道 + `POST /api/play` |
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

| 字段 | GB28181 | RTSP 拉流 | 推流设备 |
| --- | --- | --- | --- |
| key | `gb:<device_id>` | `rtsp:<source_id>` | `push:<device_id>` |
| 协议 | GB28181 | RTSP | RTMP / RTSP / WHIP；未保存协议显示"未指定" |
| 接入方式 | SIP 注册 | 拉流 | 推流 |
| 地址 / 编号 | 20 位设备编号 | 来源 RTSP 地址 | 所选协议的接入地址；旧设备为"未生成地址" |
| 状态依据 | `online`、`last_seen`（注册与心跳） | `desired_state` 是配置，`session.state` 是运行事实 | `state`：none / pending / used / expired / publishing |
| 操作 | 详情、通道、删除 | 详情、编辑、开始 / 停止拉流、预览、删除 | 详情、编辑、预览、删除；详情中重新生成地址、停止推流 |

状态含义彼此独立：GB 在线只说明注册与心跳有效；RTSP `created` 表示媒体服务器已建立拉流会话；推流 `publishing` 只表示接入地址已通过认证，画面以实际预览为准；画面的 WHEP 连接状态由实时监控的画面单独显示。

## 接口映射

| 操作 | 接口 |
| --- | --- |
| GB28181 添加 / 删除 | `POST /api/devices`、`DELETE /api/devices/{id}`（无修改接口，界面不提供编辑） |
| GB 点播 / 停止点播 | `POST /api/devices/{id}/channels/{ch}/play`、`DELETE /api/lives/{live_id}` |
| RTSP 源增删改、开始 / 停止拉流 | `/api/sources*`；密码留空不发送、填写覆盖、勾选清除或清空用户名发送空串 |
| 推流设备增删改（含协议）、重新生成地址、停止推流 | `POST /api/push-devices/credentials`（添加前预生成）、`/api/push-devices*`（`protocol`：`rtmp` / `rtsp` / `whip`，空串为未指定；创建时带预生成的 `token`）、`POST …/publish`（重新生成）、`POST …/stop` |
| 播放地址 | `POST /api/play`（`live_id` / `source_id` / `push_device_id`） |
| 浏览器播放 | `whep.js` 的 `WHEPPreview`，每个画面一个实例 |

## 当前语义（界面如实呈现）

- 播放地址：一次签发 RTSP、RTMP、HTTP-FLV、HLS、WHEP 五个 URL，共用一个一次性 token 和同一个 `expires_at`；任一地址被使用后其余失效，倒计时不代表未被使用。只有运行中的媒体流才申请地址，打开详情不点播。
- 推流设备接入地址：添加页面选择协议后由服务端预生成凭据并展示真实地址；该凭据只在内存中等待确认，发布认证只认数据库中的凭据，因此确认前不能使用，取消添加不产生设备和有效授权。确认添加（或在详情中重新生成）后凭据写入数据库，5 分钟内可首次使用；首次发布认证在数据库中原子标记已使用，并发认证只有一次成功，重启后仍不可复用；已建立的推流不受期限影响。推流中不能重新生成（不隐式断开现有发布）。地址由凭据和媒体服务器地址重建，详情页随时可查看。
- 推流协议由服务端持久化并校验，旧设备为未指定。三种地址共用一个凭据，界面只展示所选协议的地址（未指定时展示三种），服务端不限制推流协议。
- signaling 重启不恢复推流运行：媒体侧遗留的推流接收会话按对账删除，设备显示"地址已使用"。
- 实时监控：预览放入选中画面，已有视频时替换并关闭原画面的 viewer；关闭画面不停止上游点播或拉流；离开页面或 `pagehide` 关闭全部画面；不自动播放，失败不自动重连。

## 后端缺口

1. 每协议独立的一次性播放 token（当前五个 URL 共用一个）。
2. 推流协议的服务端强制约束：当前协议字段只用于展示，发布 token 不绑定协议，需要单独设计决策。
3. 推流设备的长期接入凭据（当前为一次性地址，每次接入前重新生成）。
4. GB28181 设备改名（`PATCH /api/devices/{id}`）。
5. GB 注册地址、传输方式、注册时间（区分"未注册"与"离线"）。
6. 只读统计：CPU、内存、连接数、带宽、播放会话数、各媒体流轨道与码率、观看数、服务运行时间。
7. 可选：设备 / 通道汇总接口，替代逐设备请求通道列表。

## 浏览器测试

`tests/web_console/` 使用 Node 22 内置 WebSocket 通过 Chrome DevTools 协议驱动 `google-chrome --headless`，`bidi.mjs` 通过 WebDriver BiDi 驱动 Firefox。测试针对真实 signaling 与媒体服务器，不使用模拟数据。

```
WEB_CONSOLE_URL=http://127.0.0.1:29090/ WEB_CONSOLE_DB=<signaling.db> \
PUSH_DEVICE=<正在推流的推流设备 ID> MEDIA_LOG=<media_server 日志> LEGACY_PUSH_DEVICE=<升级前创建的推流设备 ID，可选> \
node tests/web_console/cdp.mjs tests/web_console/<脚本>.mjs
```

前置条件：GB28181 模拟器注册设备 `34020000001320000101`（通道 `34020000001310000001`，可带 PCMA 音频），一台推流设备正在用 ffmpeg 推 RTMP（H.264 + AAC）。截图写入 `tests/web_console/shots/`（不纳入版本库）。

## 已知问题

- `WHEPPreview` 关闭时同时发送 DELETE 与关闭 PeerConnection，服务端常先因 DTLS 关闭释放会话，DELETE 得到 404（`whep.js` 视为已释放，资源已回收）。
- 无头 Firefox 的全新配置没有 OpenH264，WHEP offer 不含 H.264，服务端返回 400；界面显示"浏览器与服务器无法协商音视频编码"。
- 1150px 以下设备表隐藏"接入方式"列，较窄时表格在卡片内横向滚动。
