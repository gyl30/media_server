# 多协议 Web 控制台接手续审

## 接手基线

2026-10-14 在 DEV 的 `/home/gyl/example/media_server_web` 核对：

- 分支：`feature/web-console`。
- CLAUDE_HANDOFF_HEAD / 本轮 BASE_HEAD：`c5a4ae0d8011997b498387eb83097e0e80c32f50`。
- main：`dbd0e589ad66ba44cfa599535064dbf9ef958dd9`。
- origin/main 与共同祖先：`cafee7e28f96c3d8d998bb0191734a643edb9571`。
- 未提交：`signaling/web/monitor.js`、`signaling/web/player.js`，与 Claude 最后写入记录对应。
  两份原内容已保存到本轮受限证据目录，没有 reset、stash 或覆盖。
- 未初始化的 Ada/libSRTP 子模块不在本轮工作区初始化；独立构建源镜像只读引用
  主仓库已存在的第三方源码，分别核对 gitlink 为 `b12a893`、`ee1a77c`。不改变第三方。

证据目录：工作区下 `.tmp-codex-multi-input-20261014/`（不提交原始日志、凭据或截图）。

## 实际恢复结果

下表是接手时快照，不是后续功能验收。PARTIAL 表示代码已经存在，但尚缺当前代码的
完整真实回归或已确认有剩余事项；不以提交标题判定 PASS。

| 功能 | 接手状态 | 源码、提交和证据 |
| --- | --- | --- |
| GB 通道失败循环 | PARTIAL | `7afa077`、`store.js`、`channels_failure.mjs`；旧日志 9/9，需当前回归 |
| 被动 URL 预生成 | PARTIAL | `13c4d50`、`push_http.go`、`devices.js`；确认前仅 prepared 内存凭据 |
| 被动 URL 持久化 | PARTIAL | 同提交、`push_device_store.go`；DB 唯一持久化权威 |
| 300 秒有效期 | PARTIAL | 同提交；确认时生成 expires，播放 token 仍为独立 60 秒 |
| 一次性消费 | PARTIAL | DB 条件 UPDATE/RETURNING；Go 已有并发/过期/使用测试 |
| 被动地址重启 | PARTIAL | `push_device_store_test.go` 已测重开；运行会话不持久恢复 |
| RTSP 自动拉流 | PARTIAL | `ead2c88`；running 意图恢复，失败保留配置；`2b5f661` 修明确拒绝连接 |
| RTMP 主动拉流 | PARTIAL | `4a99562`；真实 librtmp 客户端 + 既有 RTMP 输入；生命周期和 decode 测试 |
| WHEP 主动拉流 | PARTIAL | `576c1f1`；真实 HTTP/ICE/DTLS/SRTP/接收/Opus→AAC；停机 DELETE 和 consent 待收口 |
| Go API | PARTIAL | 三种拉流共享 source 接口；签发入口的 media ready 判断待定向补齐 |
| 七协议添加 | PARTIAL | `devices.js` 已有全部选择，预生成交互已存在；需当前浏览器回归 |
| 详情直接播放 | NOT_STARTED | `detail.js` 仍使用五地址 `createPlaybackPanel`、跳转 monitor；`player.js` 尚未接入 |
| 单/四画面 | PARTIAL | V3 已实现；Claude 正在将画面播放器复用到详情，未提交改动未经测试 |
| 浏览器回归 | PARTIAL | 当前原始 shots/log 存在，但多数早于最新拉流提交；Firefox 历史 7/8，H.264 不可用 |
| 真实媒体测试 | PARTIAL | `pull_decode.py` 有 RTMP/RTSP/WHEP 真双轨解码；旧构建原目录已经不存在 |

## 历史证据边界

Claude transcript 最后记录为额度限制，不能作为完成声明。
其 scratchpad 当前为空；不能说旧构建、CTests 和运行环境仍可复用。
历史记录包括阶段三完整 61/61；阶段四完整 63 项首轮有 7 项失败，释放自有环境后
仅重跑这 7 项通过，不能改写成重新完整 63/63 PASS。

仓库 `tests/web_console/shots/` 的日志、图片仍存在，含运行地址和测试凭据，保持原样
受限使用，不加入报告或 Git。183 项汇总只可从历史记录恢复，后续日志曾被覆盖；32 项
布局日志仍在。均不自动代表最新 HEAD 或两份未提交文件通过。

## 接续范围

保留以上已实现功能，不再另建 RTMP/WHEP 输入或第二套设备模型。只补：

1. 详情独立 WHEP 播放、播放器对象和原有单/四画面生命周期，更新旧地址面板回归。
2. 新增输入的实际故障、关闭及授权就绪边界，先定向 RED，再最小修复。
3. 独立完整构建、Go、真实双轨、浏览器和 sanitizer 验证；保留首份失败。
4. 新增状态、抽象、配置及所有权的完整复审。

后续提交和最终测试结果另记验收报告。不合并 main，不 rebase，不推送。
