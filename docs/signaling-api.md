# 信令 API

Go 服务默认监听 `127.0.0.1:9090`，同端口提供嵌入式 Web 控制台。路由权威入口是 [`infrastructure_http.go`](../signaling/infrastructure_http.go)；启动参数见 [`config.go`](../signaling/config.go)。普通管理 API 没有内置用户登录，部署时由受控网络或网关限制访问。

JSON 请求使用 `Content-Type: application/json`；公共解码器限制 512 KiB、拒绝未知字段和额外 JSON 值。错误通常返回 `{"error":"错误码"}`。设备/通道 ID 是 20 位数字；业务源、push device 和 live/history ID 使用 UUID v4。列表、控制受理和 `media_ready` 都不能替代实际媒体解码或设备动作确认。

## 主动源与播放

| 方法和路径 | 请求与含义 |
| --- | --- |
| `GET /api/sources` | 返回 `sources` 列表 |
| `POST /api/sources` | `stream_name`、`url`，可选 `username`、`password`；创建后尝试启动 |
| `PATCH /api/sources/{source_id}` | 更新上述字段；运行源的连接参数变化会使用新 `stream_id` 重建 |
| `DELETE /api/sources/{source_id}` | 停止并删除配置 |
| `POST /api/sources/{source_id}/start`、`stop` | 改变期望状态并启动或停止 |
| `POST /api/play` | `{"source":{"source_id":"UUID"}}`，也可选择 `live_id` 或 `push_device_id`，三者必须恰好一个 |

`url` 支持 RTSP、RTMP 和 HTTP WHEP，不能嵌入用户密码。RTSP 用独立 username/password；RTMP 不接受这两个凭据字段；WHEP 的 password 字段承载 Bearer token，username 必须为空。配置持久化在 SQLite；启动时恢复期望为 running 的源，运行失败不自动证明配置已删除。

源响应含 `source_id`、`stream_name`、`desired_state` 和可选 `session`；session 包含 `stream_id`、状态和 `media_ready`。每 5 秒与媒体 receiver 对账，未知 receiver 会被清理，因此同一媒体服务应有明确的信令所有权。

播放响应包含 `stream_id`、`token`、`expires_at`，以及 `rtmp_url`、`rtsp_url`、`flv_url`、`hls_url`、`whep_url`。这些 URL 共用一张一次性票据，默认未消费有效期 60 秒；每个独立消费者重新请求。主动源和 push 必须媒体已就绪才能签票，GB live 允许先签票再等待媒体。

```sh
curl -fsS -H 'Content-Type: application/json' \
  -d '{"stream_name":"camera","url":"rtsp://192.0.2.10/live"}' \
  http://127.0.0.1:9090/api/sources

# 将响应中的 source_id 填入；媒体就绪后申请一张票据。
curl -fsS -H 'Content-Type: application/json' \
  -d '{"source":{"source_id":"替换为实际 UUID"}}' \
  http://127.0.0.1:9090/api/play
```

## 推流设备

1. `POST /api/push-devices/credentials` 预生成 token 和 RTMP/RTSP/WHIP 地址。
2. `POST /api/push-devices` 提交 `{"name":"名称","protocol":"rtmp","token":"上一步 token"}`，确认后凭据才可使用；protocol 可为 `rtmp`、`rtsp`、`whip`。
3. 使用响应中的发布地址。发布授权原子消费凭据，`stream_id` 等于该凭据；状态 publishing 只说明授权已消费，轨道就绪另看 `media_ready`。
4. `POST /api/push-devices/{id}/stop` 停止本次发布。下一次发布先调用 `/publish` 获取新凭据和新身份。

`GET /api/push-devices`、`GET /api/push-devices/{id}` 查询；`PATCH /api/push-devices/{id}` 修改 name/protocol；DELETE 删除。首次发布凭据有效期为 5 分钟，消费事实写入 SQLite；重新生成立即废弃旧凭据，发布中禁止重新生成。protocol 是地址展示选择，凭据消费本身不绑定一种发布协议。实现见 [`push_http.go`](../signaling/push_http.go) 和 [`push_device_store.go`](../signaling/push_device_store.go)。

## GB 设备与实时点播

下表中 `{d}`、`{c}` 分别是设备和通道 ID，实际路径使用对应值。

| 方法和路径 | 职责 |
| --- | --- |
| `GET/POST /api/devices` | 列表；以 `device_id`、`name` 创建设备 |
| `GET/DELETE /api/devices/{d}` | 查询或删除设备 |
| `GET /api/devices/{d}/channels` | 通道及当前 live 状态 |
| `POST /api/devices/{d}/catalog` | 请求目录；202 只表示 SIP 请求受理 |
| `POST /api/devices/{d}/query` | 设备信息/状态查询 |
| `POST /api/devices/{d}/channels/{c}/presets/query` | 查询预置位 |
| `POST /api/devices/{d}/channels/{c}/records/query` | 查询录像目录 |
| `POST /api/devices/{d}/channels/{c}/ptz` | 点动、STOP、预置位设置/调用/删除 |
| `POST /api/devices/{d}/channels/{c}/play` | 点播并返回播放票据；同通道复用 live |
| `DELETE /api/lives/{live_id}` | 停止共享 live，影响该路所有 viewer |
| `GET /api/alarms/since/{cursor}` | 增量读取内存报警，需处理淘汰造成的缺口 |

live 创建顺序为准备 RTP receiver、发送 SIP INVITE、确认 dialog，再等待媒体轨道。关闭单个 viewer 不停止共享 live。查询与 PTZ 参数及错误处理见 `device_query.go`、`gb_ptz.go`；SIP 成功响应只代表协议受理。报警仅保留最近 1000 条，重启会清空。

## 历史回放、下载和音频上传

`POST /api/devices/{d}/channels/{c}/playback` 接收 `start_time`、`end_time`（Unix 秒，结束大于开始）；`/download` 另要求 `speed` 为 1、2 或 4。返回 `playback_id`、`ps_url`、`expires_at`、`deadline`。先准备 TCP passive receiver，PS URL 的一次性播放授权被认领后才发送设备 INVITE；未认领或超时会清理。

`DELETE /api/playbacks/{playback_id}` 停止独占历史实例。处于 streaming 的回放可调用 `POST /api/playbacks/{id}/pause`、`resume`、`seek`、`scale`；seek 请求为 `{"position":相对起点秒数}`，scale 为 `{"scale":0.25或0.5或1或2或4}`，控制沿用同一 SIP dialog。绝对期限为原始范围按下载速度折算后，加票据 TTL 和 60 秒；暂停不延长期限。实现见 [`gb_playback.go`](../signaling/gb_playback.go)。

`POST /api/devices/{d}/talk` 提交 `{}`，返回 `talk_id` 和一次性 `upload_url`。上传是 WebSocket PCMA，同设备只允许一个 owner。GET 同路径查询 owner；DELETE 提交 `{"talk_id":"本次 ID"}`，只停止匹配的会话。连接成功或上传成功不表示设备已听音。实现见 [`gb_talk.go`](../signaling/gb_talk.go)。

## 媒体服务的授权回调

`POST /internal/verify` 接收 `{"operation":"play或publish","stream_id":"ID","token":"凭据"}`，成功返回 200，拒绝返回 403。配置 media control token 时使用相同 Bearer；未配置时仅允许回环调用。播放检查操作、过期、源身份及当前生命周期，成功后消费票据；发布原子消费数据库凭据。接口只供媒体服务调用，应用客户端通过上述业务 API 申请 URL。

SQLite 保存源配置、push device、GB device 和广播序号；SIP 注册、live、播放票据和报警属于运行状态。停机与资源归属见 [媒体生命周期](media-lifecycle.md)，端口和网关配置见 [构建部署](build-deploy.md)。
