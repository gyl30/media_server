# 阶段二：真实 TEST 工具验收（PASS）

全部在 TEST 的 `msperf-20261012T043240Z-76355e4` namespace 内执行；server CPU 0-7、client CPU 8-15、workers 8、地址 198.19.0.10/.11；固定预算 `free-mem≥8192MiB / free-disk≥2048MiB / server-PSS≤16384MiB`。原始结果在 TEST `/var/tmp/media_server_perf/20261012T043240Z-76355e4/phase2/`。

## 2.1 双协议媒体 smoke（PASS）

`perf_run --input rtmp --outputs flv:1,whep:1 --sources 1`，warmup 5 s、测量 20 s：

| 项 | 结果 |
|---|---|
| 总体 | `status=PASS`、`capacity_boundary=HEALTHY`、runner 退出 0 |
| 实际窗口 | flv 19.999984 s / 覆盖 0.9999935；whep 20.000085 s / 覆盖 0.9999943（要求 0.99） |
| HTTP-FLV viewer | `ready=1 progressing=1 failed=0`；稳态窗口视频 **598 条消息 / 3 968 693 B**、音频 **938 条 / 240 460 B**；首媒体 950 ms |
| WHEP viewer | `ready=1 progressing=1 failed=0`；视频 **3 647 SRTP 包**、音频 **1 001 包**、`unprotect_failures=0`、221 182 B/s；首媒体 917 ms |
| 速率核对 | 视频 598/20 s = 29.9 msg/s（=30 fps）、音频 938/20 s = 46.9/s（=48 kHz/1024）、Opus 1001/20 s = 50/s（=48 kHz/960）→ 与 fixture 一致，非“少发媒体换低 CPU” |
| 服务端 | 0.0711 cores、PSS max 28 224 KiB、FD 35、`queue_full=0 no_buffer_space=0 udp_drops=0` |
| 清理 | `all_owned_processes_reaped=true`、`remaining_owned_receivers=[]`、端口释放、无残留进程 |
| 预算采样 | 44 个 budget_samples（准备/ready/warmup/测量），未触发 |

## 2.2 负对照（5/5 PASS，逐条核对底层原始结果）

`perf_validate` 汇总 PASS，每个 case 的 runner 非零退出是预期。逐项证据：

| case | 底层证据（非汇总） |
|---|---|
| `invalid_token` | 两个独立客户端组 `flv-0`、`flv-1` **均** `failure_category=AUTHORIZATION`、`ready=0 connected=0 failed=1`、`trigger_failure_phase=authorization`、`13:Permission denied` |
| `expired_token` | 同上，两组均实际被拒绝（TTL=0 凭据） |
| `replayed_token` | 先真实 verify 消耗一次，两组再次使用同一 token 均被拒绝 |
| `source_stop` | `fault_evidence`：`media_ready_groups=2`（先健康）、请求 31868490682792080 → 实际完成 31868490690091877（7.3 ms），**落在测量窗口 [31868481628181399, 31868499628181399] 内**；publisher PID 1874377 退出码 -9；两组随后 `SOURCE_ENDED`、`2:partial message`；窗口实际长度 0.0 s 如实记录，未按请求伪造 |
| `cancel` | 在 measurement barrier 之后（起点 31868506406367806）收到 SIGTERM，`KeyboardInterrupt` 于 31868507453376996 记录，客户端 exit -15，`all_owned_processes_reaped=true` |

故障结束后无残留自有进程/receiver；负对照不参与容量 F 边界。

## 2.3 真实 Go signaling 与生命周期（PASS）

驱动 `pi_phase23.py`（本轮新增，仅测试侧），真实 `bin/signaling` + 独立 SQLite + 持久 DUT：

| 步骤 | 结果 |
|---|---|
| signaling/DUT 启动 | PASS；DUT PID 1875200、starttick 3186877626、exe 与 netns 已固定 |
| generation 0 发布 | PASS；push-device API 签发，stream_id `f08c623b…`，publisher PID/starttick 记录 |
| 当前代 viewer 播放 | PASS/HEALTHY；`flv:2`（ready 2/2、progressing 2/2、覆盖 0.99998）+ `whep:1` |
| 停止并重新发布同逻辑源 | PASS；generation 1 就绪，stream_id 变为新值（`same_stream_id=false`） |
| 新代 viewer 播放 | PASS/HEALTHY |
| 旧代 viewer 终态 | 见下方补充实验 |
| viewer churn | 3 个波次各 `flv:2,whep:1`，全部 PASS/HEALTHY，每波新 token |
| 会话与进程清理 | `perf_publish` 退出 0、`errors=[]`、`cleanup_errors=[]`；控制 API `/receivers` 为空；驱动结束后 `pgrep -x media_server` 为空 |

### 旧代 viewer 终态补充实验（`pi_phase23b.py`）

首轮驱动里旧代 viewer 的窗口恰在停源瞬间结束（相差 20 ms），**没有真正跨越停源点**，因此其 PASS 不能作为该步证据；已单独重做：

- gen0 ready 31869501051938394，持有 20 s；viewer 窗口 31869507023090892 → 31869562023090892（55 s，明确跨越停源点）。
- 结果：`status=FAIL`、`failure_category=MEDIA_STALL`、`trigger_failure_phase=runtime`、`2:partial message` → 旧 viewer 以真实媒体中断终止，**没有静默切到新 generation**。
- 两代 stream_id 不同（`fb1678da…` → `965bdd2b…`）；`devices_deleted=true`、`all_owned_publishers_reaped=true`；无残留进程。
- 该 viewer 的 `window.actual_*` 为 null（未完成测量窗口），按原始事实保留，未补造。

## 阶段二结论

工具在 TEST 上可产生真实双轨媒体、正确拒绝故障、正确处理代际与清理。允许进入阶段三。
