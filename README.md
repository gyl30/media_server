# media_server

C++ 多协议媒体服务，配套 Go 信令/Web 控制台与独立 Qt 监控客户端。支持 RTMP、RTSP、WHIP/WHEP、GB28181 PS-RTP，以及 HTTP-FLV、HLS 和有限历史 PS 输出。各方向、codec 和传输限制见协议文档。

## 文档

- [架构](docs/architecture.md)：模块职责、实时与有限媒体流转。
- [媒体生命周期](docs/media-lifecycle.md)：源、session、播放器 generation、关闭与资源归属。
- [协议](docs/protocols.md)：接入/输出、授权 URL、控制接口和 codec 边界。
- [信令 API](docs/signaling-api.md)：源、设备、一次性授权、回放、下载与音频上传。
- [构建部署](docs/build-deploy.md)：依赖、生产构建、启动、网络及维护约定。
- [性能工具](docs/performance.md)：现有工具、运行条件、测量方法和限制。

## 入口

根 CMake 构建 `media_server`，`signaling/` 是独立 Go 模块，`desktop/` 独立构建 `gb_monitor`。浏览器控制台由 signaling 提供，默认地址为 `http://127.0.0.1:9090/`；桌面使用说明见 [desktop/README.md](desktop/README.md)。

每次媒体生命周期使用全新 `stream_id`，结束后不复用；每个消费者申请独立的一次性播放票据。首帧等待自然关键帧，不提供通用 GOP/history 回放。

第一方旧测试已清理；当前构建检查不等于媒体端到端验收，历史测试结果也不代表当前版本。`.agent/` 保留历史决策与经验，开发入口见 [AGENTS.md](AGENTS.md)。本地 designs、构建产物及性能原始报告不纳入本次文档版本。
