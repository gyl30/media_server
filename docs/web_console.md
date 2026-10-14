# Web 管理控制台

signaling 通过 Go `embed` 提供 `signaling/web/` 下的静态页面（HTML、CSS、原生 ES module、SVG），没有构建步骤和外部依赖。视觉与交互依据设计稿 `designs/media-server-console`（墨绿 + 暖灰，视频区深色）。

## 页面

| 路由 | 页面 | 数据来源 |
| --- | --- | --- |
| `#overview` | 系统总览：已登记设备、在线 / 运行中、运行中媒体流、按接入类型分布、需要关注 | 快照汇总 |
| `#devices` | 设备管理：GB28181、RTSP 拉流、推流设备统一列表；搜索、类型 / 状态筛选、添加、编辑、删除 | 快照汇总 |
| `#devices/<gb\|rtsp\|push>/<id>` | 设备详情：设备信息、GB 通道与点播、RTSP 取流控制、推流地址签发、播放地址 | 快照 + `POST /api/play` |
| `#video` | 视频中心：设备树、单画面 / 四分屏 WHEP 播放 | 快照 + `whep.js` |
| `#streams` | 媒体流：可证明正在运行的流 | 快照汇总 |
| `#status` | 运行状态：接口可达性、设备与媒体流计数；资源指标标为暂不支持 | 快照汇总 |

快照（`store.js`）每 2.5 秒并行请求 `GET /api/devices`、`/api/sources`、`/api/push-devices`，再请求在线 GB 设备的 `/api/devices/{id}/channels`；新的刷新中止旧刷新，快照时间取请求发出时刻。

## 接口映射

| 操作 | 接口 |
| --- | --- |
| GB28181 添加 / 删除 | `POST /api/devices`、`DELETE /api/devices/{id}`（无修改接口，界面不提供编辑） |
| GB 点播 / 停止点播 | `POST /api/devices/{id}/channels/{ch}/play`、`DELETE /api/lives/{live_id}` |
| RTSP 源增删改、开始 / 停止取流 | `/api/sources*`；密码留空不发送、填写覆盖、勾选清除或清空用户名发送空串 |
| 推流设备增删改、签发、撤销 / 停止 | `/api/push-devices*`、`POST …/publish`、`POST …/stop` |
| 播放地址 | `POST /api/play`（`live_id` / `source_id` / `push_device_id`） |
| 浏览器播放 | `whep.js` 的 `WHEPPreview`，每个画面一个实例 |

## 当前语义（界面如实呈现）

- 播放地址：一次签发 RTSP、RTMP、HTTP-FLV、HLS、WHEP 五个 URL，共用一个一次性 token 和同一个 `expires_at`；任一地址被使用后其余失效，倒计时不代表未被使用。界面不展示 token 字段和"已被使用"状态。只有运行中的媒体流（GB live `streaming`、RTSP 会话 `created`、推流 `verified`）才申请地址，打开详情不点播。
- 推流设备：协议未指定；签发时同时给出 RTMP、RTSP、WHIP 地址，共用一个临时 token。地址只在签发响应中出现，离开详情后无法再次查看。
- 视频中心：关闭画面只释放该画面的 WHEP 会话，不停止上游点播或取流；离开视频中心或 `pagehide` 关闭全部画面；不自动播放，失败不自动重连。

## 后端缺口

以下能力在设计稿中已确定，但需要后端实现后前端才能接入：

1. 每协议独立的一次性播放 token（当前五个 URL 共用一个）。
2. 推流设备的固定接入凭据与协议字段（当前每次 `/publish` 临时签发三种协议地址）。
3. GB28181 设备改名（`PATCH /api/devices/{id}`）。
4. GB 注册地址、传输方式、注册时间（区分"未注册"与"离线"）。
5. 只读统计：CPU、内存、连接数、带宽、播放会话数、各媒体流轨道与码率、观看数。
6. 可选：设备 / 媒体流汇总接口，替代逐设备请求通道列表。

## 浏览器测试

`tests/web_console/` 使用 Node 22 内置 WebSocket 通过 Chrome DevTools 协议驱动 `google-chrome --headless`，`bidi.mjs` 通过 WebDriver BiDi 驱动 Firefox。测试针对真实 signaling 与媒体服务器，不使用模拟数据。

```
WEB_CONSOLE_URL=http://127.0.0.1:29090/ WEB_CONSOLE_DB=<signaling.db> \
PUSH_DEVICE=<正在推流的推流设备 ID> MEDIA_LOG=<media_server 日志> \
node tests/web_console/cdp.mjs tests/web_console/<脚本>.mjs
```

前置条件：GB28181 模拟器注册设备 `34020000001320000101`（通道 `34020000001310000001`），一台推流设备正在用 ffmpeg 推 RTMP。截图写入 `tests/web_console/shots/`（不纳入版本库）。

## 已知问题

- `WHEPPreview` 关闭时同时发送 DELETE 与关闭 PeerConnection，服务端常先因 DTLS 关闭释放会话，DELETE 得到 404（`whep.js` 视为已释放，资源已回收）。
- 无头 Firefox 的全新配置没有 OpenH264，WHEP offer 不含 H.264，服务端返回 400；界面显示"浏览器与服务器无法协商音视频编码"。
