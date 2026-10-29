# 流媒体核心精简与优化 性能基线（2026-10-23）

本目录归档 1ac4e04 → 04334df 一轮精简优化的 A/B 结果，以及 3cbc170 修复后的确认测量。目录被 `.gitignore` 忽略，由本地使用者管理。

## 被测二进制

| 角色 | 提交 | 远端路径（相对 rt-base） | sha256 | 本地副本 |
|---|---|---|---|---|
| 基线 | 1ac4e04 | `build/media_server` | `ef249cbbaaaa7fd62716765ede9b3f901dcc6bfa7f2133256a01abad02ca28b6` | `binaries/media_server-base-1ac4e04` |
| 最终 | 04334df | `variants/ps1/media_server` | `c48ed42f526ff606a443a70d8873aa10a88db74537fc35d7b8fca9e9ab63527d` | `binaries/media_server-ps1` |
| 修复确认 | 3cbc170 | `variants/fix1/media_server` | `7f2264e6e4d1f232812d79312fb19e6e31a8bdaf34a2c3c4574ceb8b236bbae1` | 本地 `build/media_server`（同 sha） |

- 均为 RelWithDebInfo（`-O2 -g -DNDEBUG`），gcc-16，Boost 1.92 静态库。
- ps1 在 04334df 提交前 3 分钟由同一工作区构建。与 3cbc170 构建对比 `nm -S` 的函数大小，只有 `length_prefixed_payload::get`、它的 lambda 和 `media_stream::publish` 不同，正好是 3cbc170 改动的范围。
- `result.json` 里的 `head` 字段全部是 1ac4e04。它来自运行包 `rt-base/run.sh` 的 `MEDIA_SERVER_BENCH_HEAD`，不是服务端二进制的提交；二进制身份以 `config.server_bin` 和上表 sha256 为准。
- 中间提交使用的 variants 由构建时间推断，未逐一核实，只作历史参考：udp1≈7812505，rtsp2≈ccd716b，flv1≈fe8c8ed，rtmp1/rtmp2 是 25c8607 的候选，rtspres≈1c92239，head6/gbtcp1≈7a258f8，lp1≈b06507e。all3 和 head4 是探索性组合。

## 环境

- 测试机：<TEST_HOST>，Xeon Silver 4216 16C/32T（cpu0 的超线程兄弟是 cpu16），93 GB，Ubuntu 22.04 / glibc 2.35，内核 6.8.0-84，调频策略 schedutil。
- 网络：netns `msperf-20261013T011210Z-7788320`，服务端 198.19.0.10，客户端 198.19.0.11（lo 上）。
- 运行包：`/var/tmp/media_server_tooling/claude-20261023/rt-base`，由 `bench/package_runtime.py` 生成，见 `remote/rt-base/manifest.json`。
- 夹具：`.cache/perf_fixture_720p30.flv`，sha256 `59c81471062a78e114205c96c03a99a1a3f432e50365bcd587edccd8ba029741`，H.264 Constrained Baseline 1280×720 30fps + AAC。本地副本见 `docs/verification_results/20261023-04334df-final-acceptance/fixtures/`。
- 客户端：`CLIENT_DIR=client2`。其中 `whep_fanout` 由 7812505 起的 `bench/clients/webrtc_client.cpp` 构建（UDP 接收缓冲 16 MiB），本地副本 `binaries/whep_fanout-client2`，sha256 `27b4546f…ba795`。其余客户端与 1ac4e04 运行包相同。默认的 1ac4e04 WHEP 客户端在 500 路时会出现客户端 `UdpRcvbufErrors`（见 `udp1-whep500-*`），不能用于 WHEP 500 对比。

## 命令

`remote/ab.sh <run> <server-bin> <outputs> [额外参数]` 在 netns 内执行：

```
perf_run.py --input rtmp --sources 1 --outputs <outputs> --server-bin <bin> --client-dir client2 \
  --host 198.19.0.10 --client-address 198.19.0.11 --workers 4 --server-cpus 0-3 --client-cpus 8-15,24-31 \
  --warmup 10 --duration 30 --ready-timeout 120 --ramp-per-second 200
```

mixed 场景额外加 `--duration 20`，输出为 `rtmp:20,rtsp:20,flv:20,hls:20,whep:20,gb_udp:20,gb_tcp_passive:20,gb_tcp_active:20`。`remote/prof.sh` 在测量窗口内用 `perf record -F 499 -g` 采样。

## 最终 A/B（500 路，单源，4 worker）

全部 PASS HEALTHY，progressing 500/500，`queue_full`、`no_buffer_space`、`udp_drops` 都是 0。

| 输出 | 服务端 CPU 核数 基线 → 最终 | 变化 | 服务端 PSS 峰值 MiB 基线 → 最终 |
|---|---|---|---|
| WHEP | 2.4033 → 2.2359 | −7.0% | 202.7 → 116.3 |
| GB UDP | 1.3961 → 1.1605 | −16.9% | 102.6 → 23.8 |
| HTTP-FLV | 1.0197 → 0.7522 | −26.2% | 153.8 → 100.3 |
| RTMP | 1.0844 → 0.7002 | −35.4% | 203.3 → 144.5 |
| RTSP | 1.9951 → 0.8144 | −59.2% | 152.5 → 142.2 |
| GB TCP passive | 1.0057 → 0.5451 | −45.8% | 121.0 → 144.1 |
| GB TCP active | 1.0111 → 0.5461 | −46.0% | 110.2 → 148.0 |

- mixed（04334df）：PASS HEALTHY，160/160，CPU 0.4042，PSS 58.7 MiB。
- 两个 GB TCP 场景的 PSS 峰值上升，本轮没有分析原因。
- final-base/final-final 每侧各跑一次，同一时段交替测量。同二进制重复测量的 CPU 差异最大约 1.5%（例如 lp1-rtsp500 0.8060/0.8180）。上表变化都远大于这个范围。

## 3cbc170 确认测量

ps1（04334df）与 fix1（3cbc170）同一时段交替测量，命令同上，结果见 `confirm-3cbc170.txt` 和 `remote/runs/confirm-*`。

| 场景 | 04334df 各轮 CPU 核数 | 3cbc170 各轮 CPU 核数 | 均值变化 |
|---|---|---|---|
| RTSP 500（4 轮） | 0.8135 / 0.8110 / 0.8125 / 0.8135 | 0.8208 / 0.8240 / 0.8233 / 0.8128 | +0.94%（单轮 +0.90%、+1.60%、+1.33%、−0.09%） |
| WHEP 500（2 轮） | 2.2398 / 2.2358 | 2.2274 / 2.2338 | −0.32% |
| mixed 160 | — | 0.4177，PSS 57.6 MiB | — |

- 全部 PASS HEALTHY，`queue_full`、`no_buffer_space`、`udp_drops` 都是 0，PSS 无明显变化。
- RTSP 的差异在同二进制重复测量的波动范围（约 1.5%）内，但 4 轮中有 3 轮方向一致。3cbc170 在热路径上只为每次 `get()` 多读一个 bool，每帧转换时每个 NAL 多一次比较。按约 15000 次/秒的调用量，这点开销解释不了约 0.008 核的差异，更可能是代码布局变化造成的。本轮不做新的性能优化，所以没有做周期级确认。

## 目录

- `summary.tsv`：`summarize.py remote/runs` 生成的全部 run 汇总。
- `remote/runs/<run>/`：result.json、服务端和客户端日志（去掉了 `*.credentials` 和 `*.perf.data`）。`<run>.out` 是 perf_run 的标准输出，`<run>.summary` 和 perf 报告来自 prof.sh。
- `remote/ab.sh`、`remote/prof.sh`、`remote/rt-base/{manifest.json,run.sh}`。
- `binaries/`：基线、最终二进制和 client2 的 whep_fanout。

## 非 PASS 的历史 run

- `udp1-whep500-r1/r2`：使用默认 WHEP 客户端，客户端接收缓冲溢出，因此引入了 client2。
- `prof-base-rtmp500`、`rtsp2-rtmp500-r2`：progressing 500/500，但 rtmp 组的 `measurement_end_monotonic_ns=0`，覆盖率算成 0。原因是 `bench/media_fanout.h` 客户端的竞态：各 viewer 在截止时刻自行退出并 `io.stop()`，截止定时器回调没来得及执行。这是测量工具问题，不是服务端问题，重跑即可。

## 复现

1. 在测试机上用 `bench/package_runtime.py` 打包 1ac4e04 运行包（或复用现有 rt-base）。确认夹具 sha256 一致，把被测二进制放到 `variants/<名>/media_server`，client2 的 whep_fanout 放到 `client2/`。
2. 同一时段交替执行 `CLIENT_DIR=client2 ab.sh <新run名> <bin> <outputs>`。run 目录已存在时 perf_run 会拒绝运行。
3. 用 `summarize.py` 汇总。调频是 schedutil，小幅差异要结合 perf 周期数判断。
