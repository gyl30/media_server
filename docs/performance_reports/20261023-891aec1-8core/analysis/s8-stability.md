## 8 长稳与资源回收

### 8.1 短时 churn 与清理后回收

`raw/runs/v10-settle`：单源 RTMP 300 个常驻 viewer，另有 50 个 viewer 每 15 s 断开重连，窗口 45 s；客户端全部退出后等待 20 s 再取快照（`--cleanup-settle 20`）。

| 时点 | FD | 线程 | PSS | RSS | VmSize | TCP 收/发队列 | 进程 CPU 累计 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 启动后基线 | 33 | 9 | 19.9 MiB | 21.3 MiB | 227 MiB | 0/0 | 0.04 s |
| 客户端退出后 | 32 | 16 | 89.4 MiB | 91.7 MiB | 457 MiB | 0/0 | 26.72 s |
| 再等 20 s | 32 | 16 | 89.4 MiB | 91.7 MiB | 457 MiB | 0/0 | 26.72 s |

- 会话、套接字和定时器全部回收：FD 回到基线，等待期间进程 CPU 不再增加，没有残留的定时器或重试。
- 多出的 7 个线程来自 Boost.Asio 1.92 的 `resolver_thread_pool`（`boost/asio/detail/impl/resolver_thread_pool.ipp:54-62,113-118`）：每个执行上下文在第一次 `async_resolve` 时启动 1 个解析线程，此后一直保留。服务端每个 worker 的 io_context 在签名校验（`media/http/signaling_verify.cpp:82-85`）时都会解析一次，所以上限是 worker 数 8，不会随会话数增长。
- PSS 比基线多 70 MiB，等待 20 s 没有变化。jemalloc 默认没有后台线程，脏页衰减只在分配调用时推进，进程空闲时不会归还。这是分配器保留，不是泄漏；下一轮负载会复用这些页。jemalloc 衰减参数的 A/B 结果见第 7 节。

### 8.2 30 min 长稳

负载取各协议稳定容量的约 80%～90%，单源，测量窗口 1800 s（warmup 30 s），另有一组 churn viewer 每 20 s 一轮：建立、接收 12 s、断开。窗口结束后等 30 s 再取快照。汇总用 `tools/longsum.py`（每 300 s 一段）。

| 轮次 | 常驻负载 | churn | 结果 | 服务端核（各 300 s 段） | PSS（各段范围） | FD | 实时比例最小 | lag span p50 | 清理后 FD / 线程 / PSS |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `d1-long-rtmp-7000` | RTMP 7000 | RTMP 100 × 86 轮，全部成功 | PASS | 6.78～6.80 | 2.0～2.6 GiB，无上升趋势 | 7033～7141 | 0.9999 | 233～234 ms | 32 / 16 / 2059 MiB |
| `d1-long-whep-3000` | WHEP 3000 | WHEP 50 × 87 轮，均建立并出媒体，但测量屏障失败（工具问题） | FAIL（工具） | 6.45～6.52 | 562～572 MiB | 3033～3084 | 0.9999 | 512～520 ms | 32 / 16 / 495 MiB |
| `d1-long-gb_udp-6400` | GB UDP 6400 | GB UDP 100 × 87 轮（每轮 create/delete 100 个 sender），全部成功 | PASS | 6.12～6.13 | 52～54 MiB | 12833～13035 | 0.9999 | 290～334 ms | 32 / 9 / 48 MiB |
| `d2-long-whep-3000`（`370de80` 重跑） | WHEP 3000 | WHEP 50 × 78 轮，全部成功 | PASS | 6.55～6.63 | 563～578 MiB | 3033～3084 | 0.9999 | 555～559 ms | 32 / 16 / 486 MiB |

- **RTMP 7000**：30 min 内各段服务端 CPU 相差不到 0.4%，PSS 在 2.0～2.6 GiB 间随关键帧和 churn 起伏，没有上升趋势；8600 个 churn 会话全部建立、接收、断开。lag span 233 ms，说明 30 min 内交付延迟没有累积。清理后 FD 回到基线以下（32，基线 33），进程 CPU 不再增加。
- **WHEP 3000**：3000 个常驻 viewer 全程实时，CPU、PSS、FD 平稳。87 轮 churn 的 50 个会话都完成了 ICE/DTLS 并收到媒体（`churn-*.log` 均有 `phase=media_ready ready=50`），但都在测量屏障报 `start-file time is outside the readiness deadline` 后退出：runner 预写的开始时间只比启动晚 4 s，而 WHEP 会话在负载下约 7 s 才全部出媒体。这是工具问题，已由 `370de80` 修正（第 9 节）。用新运行包重跑的 `d2-long-whep-3000` 通过：78 轮共 3900 个 churn 会话全部建立、测量、断开，CPU 除第一段外在 6.60～6.63 核，PSS 与 FD 平稳，清理后 FD 回到 32。两轮合计，服务端在 1 h 内建立并释放了 8250 个 WHEP churn 会话。
- **GB UDP 6400**：8700 次 sender 创建和删除全部成功，FD 只在 churn 的 200 个之间往复，服务端 socket 丢包和 netns `UdpRcvbufErrors` 均为 0。线程始终 9 个：GB 输出不走签名校验，没有启动解析线程。
- **清理后 PSS**：RTMP 2059 MiB、WHEP 486～495 MiB，等待 30 s 不变。原因与第 8.1 节相同：jemalloc 没有后台线程，空闲时不推进衰减，下一轮负载会复用这些页。
- **线程**：RTMP、WHEP 轮为 16 个，与第 8.1 节相同，多出的是每个 worker 的 Asio 解析线程，30 min 内不再增长。
