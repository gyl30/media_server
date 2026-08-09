# RTSP AAC RTP 边界

## 背景

核心 AAC frame 使用 ADTS，MPEG4-GENERIC RTP 的 AU payload 使用 raw AAC。原构建没有启用 vendored packetizer 的 ADTS 剥离路径，导致 ADTS 被当作 AU 数据发送；本项目自己的 RTSP demuxer能够宽容地再次剥离它，但外部客户端不应依赖这个非标准输入。

## 决策

在 ireader 编译边界启用其已有的 `RTP_MPEG4_GENERIC_SKIP_ADTS`。核心格式和 RTSP session 均不增加转换状态，也不复制第三方已经实现的 ADTS 解析逻辑。

## 验证

回归测试使用真实 RTSP muxer 生成 RTP，解析 AU header 并确认 payload 只包含 raw AAC；同一 RTP 再经过真实 RTSP demuxer 和 `avpkt2bs`，确认回到核心时仍恰好只有一个 ADTS header。
