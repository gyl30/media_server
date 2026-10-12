# perf_capacity：可复验的容量驱动

入口：`python3 bench/perf_capacity.py --help`。它调用现有 perf_run，不创建新的媒体客户端；诊断负载不自动执行。

## 参数与算法

| 参数 | 默认/约束 |
|---|---|
| `--output` | 必填、新目录 |
| `--protocols` | `rtmp`；逗号分隔，不可重复；支持 perf_run 输出协议 |
| `--start` / `--step` | `500` / `500`，按 granularity 对齐 |
| `--granularity` / `--maximum` | `100` / `10000`；正整数，start 不大于 maximum |
| `--healthy-repeats` / `--failed-repeats` | `3` / `2`，边界额外独立复验次数 |
| `--round-timeout` | `900` 秒；到期 SIGTERM 自有 runner 进程组，30 秒未退出则 SIGKILL |
| `--counts` | 显式逗号整数档位，跳过搜索；各档不少于协议数量 |
| `--plan-only` | 只保存 NOT_RUN 计划，不运行服务 |
| `-- 后的参数` | 透传 perf_run；禁止覆盖 `--output`、`--outputs`、`--fault` |

总 viewer 数平均分配到协议，余数顺序分配。多协议搜索的 start/granularity 应保证搜索中的最低候选能够为每种协议安排至少一个 viewer。

搜索：按 step 递增，首次确认 SERVER_UNSTABLE 后按 granularity 二分缩小 H/F 区间；然后分别执行规定次数的独立 H/F 复验。任何 CLIENT_RESOURCE、RESOURCE_BUDGET、RUNNER_FAILED、未确认失败或不一致复验都会 INCONCLUSIVE，不能充当服务端 F 边界。

## 不启动负载的示例

```bash
python3 bench/perf_capacity.py \
  --output /var/tmp/capacity-plan-001 \
  --protocols rtmp,rtsp --start 10 --step 10 \
  --granularity 2 --maximum 40 --plan-only
```

这是计划验证，不是容量测试。Pi 在独立 TEST 环境才执行正式搜索，例如：

```bash
python3 bench/perf_capacity.py \
  --output /var/tmp/perf-capacity-whep-001 \
  --protocols whep --start 100 --step 250 --granularity 50 \
  --maximum 3000 --healthy-repeats 3 --failed-repeats 2 \
  --round-timeout 900 -- \
  --input rtmp --sources 1 --warmup 30 --duration 90 \
  --host 198.19.0.10 --client-address 198.19.0.11 \
  --workers 8 --server-cpus 0-7 --client-cpus 8-15 \
  --min-free-memory-mib 8192 --min-free-disk-mib 10240
```

以上档位是可执行示例，不是已批准的压力规模或已测容量；Pi 必须先检查 host、安全预算、客户端余量，按手册批准的负载执行。该搜索步长250/granularity50/start100符合参数约束。

## 结果与失败

- `round-NNN-count/result.json` 保留每轮 perf_run 原始结果；同级 `.runner.log` 保存驱动输出。失败轮不被覆盖。
- 根 `result.json` 每轮更新 checkpoint，完成后写最终搜索/固定档位汇总。运行中的 checkpoint 为 NOT_RUN，不等于成功。
- 缺失/损坏 JSON、未知状态、退出码矛盾、FAIL 却宣称 HEALTHY 或 PASS 却宣称 SERVER_UNSTABLE 均记 `INCONCLUSIVE/RUNNER_FAILED`。
- SIGTERM/SIGINT 保存取消证据及已完成轮次，停止自有进程组。取消不是服务端容量失败。
- 固定 `--counts` 也执行安全停止：只有 HEALTHY 或明确 SERVER_UNSTABLE 轮次允许继续后续预先指定档位；RESOURCE_BUDGET、CLIENT_RESOURCE、UPSTREAM_FAILED、PROCESS_IDENTITY、RUNNER_FAILED 等均立即结束，保留已完成轮次及 `reason`，返回 INCONCLUSIVE。SERVER_UNSTABLE 后继续仅用于操作员明确批准的独立超载诊断，不能由此放宽健康判据。
- 达到 maximum 仍无 F 时返回 `INCONCLUSIVE/NO_FAILURE_BOUNDARY`，健康轮次只能提供已验证容量下界，不是软件最大值。
- 输出的 `overload_diagnostic_counts` 只是候选计划，不会自动加压，不保证可安全执行。

PASS 表示按当前算法得到并复验边界；不是宣称系统完全无泄漏或全部浏览器兼容。profiling 必须另起同条件独立运行，不能与无 profiler 正式测量混合。

本轮只执行 CLI、计划及 mock/offline 回归，未执行正式容量搜索：CODE/DEV VERIFIED；正式 TEST 搜索 NOT_RUN，交由 Pi。参见 [perf_run](perf_run.md)、[结果格式](result_format.md)、[Pi 手册](pi_handoff.md)。
