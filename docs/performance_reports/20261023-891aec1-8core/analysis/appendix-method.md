## 附录 A 方法与判定

- 负载：单源 H.264 CBP 720p30 2 Mbps + AAC 48 kHz（`perf_fixture_720p30.flv`，GOP 2 s），每 viewer 持续接收音视频；编解码矩阵另用 H.265/G.711/Opus fixture（`raw/env/test-host-env.txt` 末尾有 SHA256）。
- 一轮：服务端、上游实例、签名桩由 `perf_run.py` 拉起；viewer 以 200/s 爬坡，全部就绪后 warmup 15 s，测量窗口 60 s。窗口内每秒采样一次进程 CPU/PSS/FD，每 5 秒采一次套接字队列；perf stat 统计服务端核的 cycles/instructions。
- 健康：每组 ready=progressing=请求数、客户端正常退出、窗口覆盖率 ≥0.99、无 UDP drop/队列满、清理无残留，且每个 viewer 的媒体时间推进/墙钟 ≥0.98（`viewer_realtime_min`）。
- 服务端饱和（`SERVER_SATURATED`）：窗口内服务端核平均繁忙 ≥90%（或单核 ≥98%、单线程 ≥95%）且出现实时落后或失败；或准备阶段失败且准备阶段服务端核达到 95%/单核 98%，同时客户端核未饱和、失败不属于客户端资源。鉴权 403 只有在签名桩零拒绝、处理耗时 <1 s 时才计入服务端侧（服务端 3 s 鉴权截止超时）。
- 容量：`perf_capacity.py` 递增、二分，再对健康点重复 3 次、失败点重复 2 次；未能重复的边界另行用固定计数确认，结果按实际通过次数报告。
- 成本口径：schedutil 使频率随负载在 1.0～2.7 GHz 间变化，CPU 时间随频率缩放。跨负载比较用周期数（APERF/MPERF 与 perf stat 交叉核对）和指令数；容量结论以满载（约 2.7 GHz）时的实测为准，不由低负载线性外推。
- 回环口径：服务端与客户端在同一 netns 的 lo 上。服务端发出的包在服务端核的 softirq 中完成接收侧协议处理（UDP/TCP 查找、入队、唤醒客户端 epoll），这部分在真实网卡部署时发生在客户端机器；客户端发往服务端的 TCP ACK 则在客户端核上处理。报告中单列服务端 softirq，作为回环额外成本的上界估计。
