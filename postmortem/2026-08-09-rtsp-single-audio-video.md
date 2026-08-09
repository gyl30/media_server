# RTSP 单音视频选轨边界

## 背景

核心和输出协议只需要处理每路流的一条视频与一条音频。原 RTSP 输入会 SETUP SDP 中全部受支持的 media，但随后把所有视频映射到同一个 track、所有音频映射到另一个 track，导致重复同类 media 交错使用同一轨道和转换器缓存。

## 决策

选轨属于 RTSP SETUP 边界。按 SDP 顺序保留第一条受支持的视频和第一条受支持的音频，其余 media 交给 ireader 的既有忽略机制删除。判断直接利用 ireader 已压缩的已选 media 前缀，不增加选轨状态，也不在核心或输出协议增加多轨过滤层。

## 验证

回归测试通过真实 TCP、RTSP client 和 SDP parser 输入不支持及重复的音视频 media，确认只对首个受支持的 H.265 和 AAC 发出 SETUP，随后直接进入 PLAY。
