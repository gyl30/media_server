# 性能测试中发现的功能问题

只定位、记录和评估影响，未修改生产代码。原始数据位于 `raw/runs/`。

## F1 WHEP 拉流输入的 FLV/RTMP 视频 DTS 以半速推进

现象：

- `v4-in-whep_pull`：同一源的 RTMP、HTTP-FLV viewer 的 `viewer_realtime_min=0.4958`；RTSP、WHEP、GB 为 1.0024。帧率正常：FLV 每 viewer 20 秒收到 601 个视频帧，ffprobe 6 秒解码 181 帧。
- `v5-in-whep_pull`：复现，`realtime_lagging_groups=['rtmp-0','flv-0']`，服务端 CPU 未饱和，判为 FAIL/UNCONFIRMED。
- `d1-whep_pull/decode-flv.log`：FLV 输出 900 帧，pts 跨度 29977 ms，dts 跨度 14834 ms；每 150 帧 dts 只前进 2475 ms（16.5 ms/帧），`pts - dts` 从 6.6 s 增至 21.7 s，持续增长。pts 增量出现 20～47 ms 的跳变。
- 对照 `d1-whip`（WHIP 输入，同一接收实现，但发布端不发 SR）：dts 等于 pts，pts 增量只有 33/34 ms。

根因链：

1. `media/webrtc/whip_media_receiver.cpp:262-287` `apply_sender_report` 每收到一个 RTCP SR，就按 NTP 差值重设 RTP→pts 映射（`rtsp_demuxer_set_timestamp`）。
2. 上游是本服务的 WHEP 输出，SR 由 `third/ireader/librtp/source/rtcp-sr.c:86-89` 生成：SR 中的 RTP 时间戳 = 最后发送包的 RTP 时间戳 + 距该包发送时刻的墙钟差。它反映发送时刻，发送抖动会进入映射，重设后 pts 出现前后跳变。
3. `third/ireader/avbsf/src/avdtsinfer.c:18-24` 把任何小于已见最大 pts 的视频帧视为 B 帧，此后 `max_bframes` 不再归零；`:50-61` 按 `fps1000 = I/P 间隔/(bframes+1)` 推算 dts，结果是 16.5 ms/帧，并钳制 `dts <= pts`。一次足够大的回跳就使该源此后的 dts 永久半速。
4. FLV tag 时间戳取 dts（`media/flv/flv_tag_output.cpp:143-144`），所以 RTMP/HTTP-FLV 的时间线半速推进、CompositionTime 持续增长；RTSP/WHEP/GB 的 RTP 时间戳取 pts，不受影响。

影响评估：

- 范围：WHEP 拉流输入（尤其级联拉取本服务或其他按发送时刻生成 SR 的 WHEP 源）。RTSP 发布 `media/rtsp/rtsp_publish_media.cpp:108-128` 有同样的 SR 重设逻辑，发布端 SR 与媒体时间不一致时也有风险；本轮 ffmpeg RTSP 发布未触发。RTSP 拉流不重设，不受影响。
- 输出：RTMP、HTTP-FLV 直接受影响。HLS 和 GB PS 同样写入 dts（`media/hls/hls_segmenter.cpp:75`），TS/PS 中 PTS-DTS 会无限增大；本轮只验证了 ffprobe 能解码 HLS，未验证播放器长时间播放。
- 时间：FLV CompositionTime 为有符号 24 位毫秒（约 8389 s），按每秒增长 0.5 s 推算，约 4.7 小时后溢出。在此之前，依赖 dts 的播放器、录制和转封装可能出现缓冲异常或时间线错误。
- 严重度：中高。属于功能正确性问题，与性能无关；性能矩阵中该单元格记为 FAIL。

修复方向（待决策，未实施）：只在首个 SR 建立 NTP 对齐、后续不再重设映射；或者无 B 帧源直接令 dts=pts，避免依赖 ireader 的 dts 推断。

## F2 HLS 输出 G.711 使用私有 TS stream type

现象：`v4-codec-h264_pcma-rtmp`、`v4-codec-h264_pcmu-rtmp`、`v4-codec-h265_pcma-rtmp` 中 RTMP/RTSP/FLV 的解码探测都能解出 G.711（`pcm_alaw` 282 帧/6 s），HLS 的音频流被 ffprobe 识别为 `mp3`，解出 0 帧。

原因：`media/hls/hls_segmenter.cpp:295-301` 使用 `PSI_STREAM_AUDIO_G711A/U`（0x90/0x91，GB/T 25724 私有类型，`third/ireader/libmpeg/include/mpeg-proto.h:100-101`）。HLS 规范与通用播放器（Safari、hls.js、ExoPlayer）都不支持 TS 内的 G.711。

影响：G.711 源的 HLS 播放只有视频，音频不可用。Opus 源在 HLS 中被明确跳过（日志 `hls skip unsupported track`），G.711 却输出了不可用的轨。严重度：中（兼容性）。可以和 Opus 一样跳过，或在文档中声明 HLS 不支持 G.711。

## F3 观测与接口语义

- 1 核过载（`v3/v5/v6-neg-rtmp-1cpu-2000`）时，RTMP play 会话因 1 MiB 写队列高水位（`media/net/tcp_transport.cpp:55-60`，`no_buffer_space`）被关闭，但 `media/rtmp/rtmp_session.cpp:45-51` 只调用 `shutdown()`，没有日志。perf_run 的 `no_buffer_space` 证据计数为 0，过载断开只能从客户端 EOF 推断。严重度：低（可观测性）。
- GB sender 创建在源编码不受 PS 支持时（如 Opus）返回 HTTP 500 `operation_failed`（`media/http/gb28181_http.cpp:156-159`），与内部错误无法区分。严重度：低（接口语义）。

## F4 接近饱和时同 worker 的控制面与新会话被媒体分发独占

现象（`raw/runs/diag3-gb_udp-8000`、`raw/diag/diag3-gb_udp-8000/probe.txt`）：

- GB UDP 8000 sender，窗口内服务端 7.44 核、0.5 s 区间最高 7.98 核；全部 viewer 实时（`viewer_realtime_min=0.9969`）。
- 同期对服务端 HTTP 端口的 404 探测：多数 5～300 ms，每 8～12 s 出现 0.8～1.8 s 尖峰；窗口结束 32 组同时删除 sender 时，一次探测 20 s 超时、一次 10.9 s。
- 清理：每组顺序删除，7500 时全部 7500 个删除在 4.4～4.8 s 内完成；8000 时各组在 11～38 s 内只删除 201/8000 个，客户端 10 s 请求超时后停止。删除本身只是摘除登记并 `shutdown()`（`media/http/gb28181_http.cpp:288-294`），开销很小。

根因：`media/core/worker_sink_dispatcher.h` 的 `sink_group::drain()` 在一个 handler 内循环到 `pending` 为空才返回（`for (;;)`）。worker 媒体负载接近 100% 时，新帧到达速度接近处理速度，这个 handler 会连续运行很久，同一 io_context 上的其他 handler（HTTP 请求、accept、播放鉴权回调、其他会话读写）都要等它让出。HTTP 连接按轮询分到各 worker（`service.cpp:44-69`），所以控制请求的延迟由所在 worker 的分发积压决定。

影响：

- 媒体面在 8000 时仍实时，但控制面不可用，按服务端饱和计 FAIL；GB UDP 容量取 7500 一侧。
- WHEP 3750（`raw/capacity/cap6b-whep-confirm/round-003/004`）：爬坡时服务端核 100%，一组 256 个 viewer 的建立耗时 73 s，末尾 viewer 出现 `STUN nomination timeout`。新会话的 ICE/DTLS 处理排在饱和 worker 的分发之后。
- GB TCP active 14000（`raw/capacity/cap2c-gb_tcp_active/round-006-14000`）与 1 核 GB TCP passive 2500（`raw/runs/v12-neg-gb_tcp-1cpu-2500`）：爬坡时服务端核 100%，最后一组的 `/gb28181/sender/create` 超过客户端 10 s 请求时限（客户端记为 `14:Bad address`），服务端日志没有创建失败记录。媒体面并未失败，失败的是控制请求。
- 正常负载（≤ 85%）下不明显；它决定的是饱和边缘的行为，而不是吞吐上限。

方向（未实施）：`drain()` 每次只处理一批，仍有积压时重新 `post` 自身，把执行权交还 io_context。不改变分发模型和 D03/D06 的所有权，只增加每批一次 post。它不能提高吞吐，只能让控制面和新会话在饱和边缘得到公平调度。

## F5 饱和后同 worker 的 TCP viewer 集体断开，断开前内存膨胀

现象：

- `raw/capacity/cap7-rtmp/round-004-9000`：9000 个 RTMP viewer 全部就绪后，窗口内 2217 个收到服务端 EOF。36 个客户端组各断开 61～67 个（约 1/4），viewer 按 accept 轮询分到 8 个 worker，相当于 2 个 worker 上的 viewer 全部断开。服务端 PSS 峰值 13.3 GB（8500 健康轮次为 4.1～5.1 GB），日志没有任何断开记录。
- RTSP 8250/8500（`raw/capacity/cap2b-rtsp/round-007/008`）同样表现为服务端主动 EOF、PSS 11～13 GB。
- 多源同样发生：`raw/capacity/cap3-rtmp-s100/round-008-9000`、`round-009-8750`（100 个源）各组 77～113/256 个 viewer 被服务端 EOF，PSS 13.6/12.3 GB；`cap3-mix/round-010-8000` 在测量前 RTMP、RTSP 各组即出现 EOF，`round-011-7750` RTSP 每组 63～64 个断开，同一轮 WHEP 和 GB 组不受影响。

机制（由代码推断，未逐 viewer 打时间戳证实）：

1. `sink_group::drain()`（`media/core/worker_sink_dispatcher.h:183-217`）在饱和 worker 上长时间不返回（见 F4）。
2. drain 对每个 viewer 调用 `tcp_transport::write()`（`media/net/tcp_transport.cpp:26-37`），它只是复制数据并 `post` 到同一 worker。drain 不返回，这些 handler 就留在 io_context 队列里；`queued_write_bytes_` 在 `safe_write` 执行时才累加，所以排队数据不受 1 MiB 高水位约束，PSS 随积压线性增长。
3. drain 让出后，积压的 `safe_write` 依次执行，同一 viewer 很快超过 1 MiB，回调 `no_buffer_space`，`rtmp_session` 静默 `shutdown()`；或者跨 worker 的 `pending` 超过 2500 帧（`worker_sink_dispatcher.h:84-89,154`），整个 sink group 被 `on_end()`。两条路径都没有日志，按 worker 成批发生。

对照：GB TCP sender（`media/gb28181/gb28181_tcp_sender_session.cpp:117-135`）把数据追加到会话自己的 `output_`，只在 `output_` 为空时 post 一次 flush，积压期间多帧合并为一次写。饱和时它的每 viewer 周期从 2.6 降到 1.55 Mcycles，viewer 不断开、只是延迟增大（`cap2c-gb_tcp_active`、`cap2d-gb_tcp_passive`）。

影响：RTMP/RTSP/HTTP-FLV 超过饱和点后不是逐步降级，而是一个 worker 上的所有 viewer 同时断开，断开前占用数 GB 内存。在 `--max-server-pss-mib` 较小或与其他服务共存的部署中，这段膨胀可能先触发 OOM。严重度：中（过载行为和可观测性），不影响饱和点以下的正常运行。

## F6 UDP 媒体输入使用系统默认接收缓冲，多源时关键帧突发丢包

现象：

- `raw/runs/in-gb_udp-300`：300 路 GB UDP 输入，每源 1 个 FLV viewer。服务端只用 2.41 核（30%，单核最高 0.35），窗口内服务端 UDP socket 丢包 204 个（`server_evidence.udp_drops`，每 5 s 采样从 156 匀速增至 360，爬坡阶段已有丢包），判 SERVER_UNSTABLE。同样配置 100 路（`in-gb_udp-100`）丢包 0。
- `raw/runs/in-whep_pull-100`：100 路 WHEP 拉流输入（SRTP over UDP），服务端 5.90 核、单核最高 0.75，丢包 813。
- `in-rtsp_udp-100`：100 路 RTSP UDP 发布，丢包 0。
- H.265 + AAC 经 GB UDP 输入、同时带全部 8 种输出（每种 5～20 个 viewer）：4 轮中 2 轮 HLS 首个分片 10 s 内未生成（`hls playlist not ready`，503），runner 中止（`raw/runs/b2-codec-h265_aac-gb_udp`、`-r3`）；另 2 轮通过（`-r2`、只开 HLS 的 `-hlsonly`）。两轮失败的 netns 级 `UdpRcvbufErrors` 整轮增量为 110 和 108，两轮通过为 0；同一夹具经 GB TCP active/passive 输入、H.264 夹具经 GB UDP 输入都没有丢包。H.265 夹具关键帧平均 132 KB、最大 142 KB（H.264 夹具平均 97 KB），约 100 个 RTP 包，丢包数正好约等于一个关键帧。客户端 UDP socket 都设置了大接收缓冲（`bench/clients/gb_client.cpp:200`），所以推断丢包发生在服务端 GB UDP 接收 socket 上：H.265 关键帧突发超过了 212992 字节的默认缓冲。HLS 分片要从关键帧开始（`media/hls/hls_segmenter.cpp:43-49`），播放列表等待 10 s 仍无分片时返回 503（`media/http/hls_http_session.cpp:144-160`），HLS 客户端因此退出；runner 随即中止，其他输出组还没进入测量，无法判断它们是否受影响。为什么一个关键帧丢失就让首分片超过 10 s，本轮没有逐帧追踪，原因未完全确认。
- 带采样的诊断轮（`raw/runs/p2-in-whip-100`、`p2-in-gb_udp-200`，开着 perf record、syscount、offcputime）：WHIP 100 路丢包 244、GB UDP 200 路丢包 12，服务端分别只用 5.35 和 1.78 核；同配置不采样的 `in-whip-100` 丢包为 0。很小的额外延迟就足以溢出，说明默认接收缓冲几乎没有余量。
- WHIP 输入梯度（`in-whip-100/150/200`）：100、150 路丢包 0；200 路丢包 98783，FLV viewer 在测量前因断流失败（判 SERVER_UNSTABLE）。200 路时 8 个 worker 线程各 0.72～0.87 核，服务端 12.7 Gcycles/s（2.08 GHz），只有满载能力 21.6 G 的 59%。指令数随源数线性增长（每源 134～141 Minstr/s），CPU 时间却维持在 5.7～6.0 核，schedutil 只把频率升到 1.2～2.1 GHz。

原因（推断）：

- `media/net/udp_transport.cpp:16-40` 打开并绑定 socket，没有设置 `SO_RCVBUF`，接收缓冲取主机默认的 `net.core.rmem_default = 212992`。
- 测试素材关键帧平均 88 KB、最大 118.5 KB（ffprobe），约 85 个 RTP 包。回环 skb 的 truesize 每包约 2.3～2.5 KB，一个关键帧突发就接近 208 KiB 上限。worker 逐包经 reactor 完成读取，只要读取稍慢于突发到达（例如正在处理其他源的帧），就会溢出；worker 平均只有 35% 繁忙也会发生。源越多（每 worker 37 个源对 12 个），突发与处理重叠的概率越大，所以 300 路丢包而 100 路不丢；WHIP 和 WHEP 拉流每源还要在 worker 上同步做 SRTP 解密和 Opus→AAC 转码（每个 20 ms 音频帧一次），worker 被成段占用，接收更容易落后：WHEP 拉流 100 路、WHIP 200 路即出现丢包，远在 CPU 饱和之前。
- 没有在本轮做 `SO_RCVBUF` 或 netns `rmem_default` 的对照（前者要改生产代码，后者要改测试环境配置），机制未直接证实。

影响：关键帧丢包会造成解码花屏，直到下一个关键帧（本素材 GOP 2 s）。真实网卡同样会成串到达关键帧突发，主机默认 `rmem_default` 也常为 208 KiB，因此不是回环特有现象。多路 GB UDP/RTSP UDP/WHIP/WHEP 拉流接入时会遇到。严重度：中。

方向（未实施）：为媒体接收 socket 设置较大的 `SO_RCVBUF`（例如 2～4 MiB，受 `rmem_max` 限制），配合 `udp_drops` 观测验证。属于 transport 内部的 socket 选项，符合 D04 的网络所有权划分。
